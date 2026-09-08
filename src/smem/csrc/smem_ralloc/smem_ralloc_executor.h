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
#ifndef MEMFABRIC_HYBRID_SMEM_RALLOC_EXECUTOR_H
#define MEMFABRIC_HYBRID_SMEM_RALLOC_EXECUTOR_H

namespace ock {
namespace smem {
/*
 * FAR side executor of ralloc: handles SMEMRA_RPC_OP_JOIN_ALLOC (create-or-extend).
 * The first request of one pool builds the parity entity (same id, same window options,
 * requested size as initial local commit) and joins the group; later requests extend one
 * more block on the local slot. Replies with the gva of the contributed block strictly
 * after the group barrier returned, so the requester can use it at once.
 */
class SmemRallocExecutor {
public:
    static SmemRallocExecutor &Instance();

    SmemRallocExecutor() = default;
    ~SmemRallocExecutor() = default;

    SmemRallocExecutor(const SmemRallocExecutor &) = delete;
    SmemRallocExecutor(SmemRallocExecutor &&) = delete;
    SmemRallocExecutor &operator=(const SmemRallocExecutor &other) = delete;
    SmemRallocExecutor &operator=(SmemRallocExecutor &&other) = delete;

    /* registers JOIN_ALLOC handler into the rpc service */
    static Result Start();

    static void Stop();

private:
    static Result OnJoinAlloc(SmemRallocRpcMsg &msg);
};
} // namespace smem
} // namespace ock

#endif // MEMFABRIC_HYBRID_SMEM_RALLOC_EXECUTOR_H
