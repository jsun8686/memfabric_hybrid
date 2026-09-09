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
#ifndef MEMFABRIC_HYBRID_SMEM_RALLOC_MASTER_H
#define MEMFABRIC_HYBRID_SMEM_RALLOC_MASTER_H

#include <map>
#include <mutex>

#include "smem_config_store.h"
#include "smem_ralloc_rpc_def.h"

namespace ock {
namespace smem {
/*
 * Master service of ralloc: pure rpc service, not a member of any pool.
 * Op handlers are registered by the rpc service on every node; the process hosting the
 * config store server activates the master role during smem_ralloc_init: publishes its
 * rpc endpoint under the RA_ prefixed store, keeps the candidate table of FAR nodes
 * (with their last reported committed bytes) and answers PLACEMENT with the least
 * committed node.
 */
class SmemRallocMasterService {
public:
    static SmemRallocMasterService &Instance();

    SmemRallocMasterService() = default;
    ~SmemRallocMasterService() = default;

    SmemRallocMasterService(const SmemRallocMasterService &) = delete;
    SmemRallocMasterService(SmemRallocMasterService &&) = delete;
    SmemRallocMasterService &operator=(const SmemRallocMasterService &other) = delete;
    SmemRallocMasterService &operator=(SmemRallocMasterService &&other) = delete;

    /* store: RA_ prefixed store; selfEp: local rpc endpoint; seedSelf: seed self as placement
     * candidate, only meaningful when the node role is FAR */
    Result Start(const StorePtr &store, const SmemRallocRpcEndpoint &selfEp, bool seedSelf);

    void Stop();

    bool IsRunning() const;

    /* rpc dispatch entries, invoked by the rpc service lambdas on every node */
    Result OnRegister(SmemRallocRpcMsg &msg);

    Result OnPlacement(SmemRallocRpcMsg &msg);

private:
    struct Candidate {
        SmemRallocRpcEndpoint ep{};
        uint64_t committedBytes = 0;       /* committed bytes on the HOST media, last reported value */
        uint64_t deviceCommittedBytes = 0; /* committed bytes on the DEVICE media, last reported value */
    };

    std::mutex mutex_;
    std::map<uint32_t, Candidate> candidates_;
    bool running_ = false;
};
} // namespace smem
} // namespace ock

#endif // MEMFABRIC_HYBRID_SMEM_RALLOC_MASTER_H
