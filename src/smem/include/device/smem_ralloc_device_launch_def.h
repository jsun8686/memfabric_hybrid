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

#define SMEM_RALLOC_DEVICE_COPY_BATCH_SEG_MAX 16

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

#ifdef __cplusplus
}
#endif

#endif /* __MEMFABRIC_SMEM_RALLOC_DEVICE_LAUNCH_DEF_H__ */
