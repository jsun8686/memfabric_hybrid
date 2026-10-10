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

/* Private launch ABI shared between the install-time built kernel library
 * (smem_ralloc_device_kernel.cpp, bisheng) and the main-build host loader
 * (smem_ralloc_device_rdma.h/.cpp). Pure POD, no dependency, so both sides can
 * include it (the host side via the smem include/device path, the kernel side
 * via -I../include/smem/device at install time).
 *
 * One batch segment carries the host-side precheck verdicts the kernel cannot
 * derive by itself: the peer rank (SQ selection and mr[] rkey lookup) and the
 * direction (WQE opcode). Longer batches are split by the C layer into
 * consecutive launches of at most SMEM_RALLOC_DEVICE_COPY_BATCH_SEG_MAX segments.
 */
#ifndef __MEMFABRIC_SMEM_RALLOC_DEVICE_LAUNCH_DEF_H__
#define __MEMFABRIC_SMEM_RALLOC_DEVICE_LAUNCH_DEF_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 64 segments x 32B = 2KB, comfortably inside the kernel launch parameter area
 * (~4KB) — the table still travels by value, so no GM staging buffer is needed
 * (a pointer-based table would race the host runahead: the ring would have to be
 * reused before in-flight kernels finished reading it). Measured effect of the
 * chunk size: each chunk kernel costs one launch + one quiet (per peer), so 64
 * vs 16 cuts that amortized overhead 4x on large batches. */
#define SMEM_RALLOC_DEVICE_COPY_BATCH_SEG_MAX 64

struct smem_ralloc_device_batch_seg {
    uint64_t src;     /* device window GVA, local slot when write, peer slot when read */
    uint64_t dst;     /* device window GVA, peer slot when write, local slot when read */
    uint64_t size;    /* bytes of this segment */
    uint32_t peerRank; /* resolved by the host precheck: SQ index / mr[] row */
    uint32_t isWrite; /* 1: local -> peer one-sided WRITE, 0: peer -> local one-sided READ */
};

struct smem_ralloc_device_batch_args {
    struct smem_ralloc_device_batch_seg segs[SMEM_RALLOC_DEVICE_COPY_BATCH_SEG_MAX];
    uint32_t count;    /* valid segments, 1 .. SEG_MAX */
    uint32_t entityId; /* ralloc pool entity id in the device meta window */
};

/* DVA single-submit variant of the routed batch: the five descriptor arrays (SoA layout,
 * see smem_ralloc_batch_copy_v2_params) live in DEVICE memory; the host passes their
 * device addresses and the kernel loads each segment's fields from global memory. One
 * launch covers the whole batch regardless of count -- there is no SEG_MAX chunking and
 * no per-chunk pipeline drain, the per-lane SQ is kept safe by an in-kernel sub-batch
 * quiet every SMEM_RALLOC_DEVICE_DVA_LANE_BUDGET segments (the SQ ring depth is 8192). */
#define SMEM_RALLOC_DEVICE_BATCH_MAX_COUNT (1U << 20) /* descriptor count hard bound */
#define SMEM_RALLOC_DEVICE_DVA_LANE_BUDGET 2048U      /* segments per lane between quiets */

struct smem_ralloc_device_batch_dva_args {
    uint64_t srcArray;   /* device address of the const uint64 sources[count] */
    uint64_t dstArray;   /* device address of the const uint64 destinations[count] */
    uint64_t sizeArray;  /* device address of the const uint64 dataSizes[count] */
    uint64_t rankArray;  /* device address of the const uint32 peerRanks[count] */
    uint64_t writeArray; /* device address of the const uint32 isWrites[count] */
    uint32_t count;      /* valid segments, 1 .. SMEM_RALLOC_DEVICE_BATCH_MAX_COUNT */
    uint32_t entityId;   /* ralloc pool entity id in the device meta window */
    /* SQ doorbell batching: publish every dbBatch WQEs with one doorbell (PI semantics)
     * instead of ringing per WQE; also flushed on peer switch / lane-budget quiet / batch
     * end. 0 selects the host default from MF_DVA_DB_BATCH (1 = per-WQE doorbell, the
     * historical behavior). Clamped to SMEM_RALLOC_DEVICE_DVA_LANE_BUDGET so a batch never
     * exceeds the outstanding-WQE bound the in-kernel quiet relies on. */
    uint32_t dbBatch;
};

#ifdef __cplusplus
}
#endif

#endif /* __MEMFABRIC_SMEM_RALLOC_DEVICE_LAUNCH_DEF_H__ */
