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
#include "smem_ralloc_master.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "smem_logger.h"
#include "smem_ralloc_rpc.h"

namespace ock {
namespace smem {
SmemRallocMasterService &SmemRallocMasterService::Instance()
{
    static SmemRallocMasterService instance;
    return instance;
}

Result SmemRallocMasterService::Start(const StorePtr &store, const SmemRallocRpcEndpoint &selfEp, bool seedSelf)
{
    if (running_) {
        SM_LOG_WARN("ralloc master service already running");
        return SM_OK;
    }
    SM_VALIDATE_RETURN(store != nullptr, "store is null", SM_INVALID_PARAM);

    /* publish master endpoint so that other nodes can discover it */
    std::vector<uint8_t> epData(sizeof(SmemRallocRpcEndpoint));
    (void)memcpy(epData.data(), &selfEp, sizeof(SmemRallocRpcEndpoint));
    auto ret = store->Set(SMEMRA_RPC_MASTER_STORE_KEY, epData);
    SM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "publish master endpoint failed: " << ret);

    /* op handlers live in the rpc service on every node, activation here means:
     * publish the endpoint above and optionally seed self as placement candidate (FAR only) */
    {
        std::lock_guard<std::mutex> guard(mutex_);
        candidates_.clear();
        if (seedSelf) {
            Candidate self{};
            self.ep = selfEp;
            candidates_.emplace(selfEp.rankId, self);
        }
    }

    running_ = true;
    SM_LOG_INFO("ralloc master service started, endpoint: " << selfEp.ip << ":" << selfEp.port
                                                            << " rank: " << selfEp.rankId
                                                            << " seedSelf: " << seedSelf);
    return SM_OK;
}

void SmemRallocMasterService::Stop()
{
    if (!running_) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(mutex_);
        candidates_.clear();
    }
    running_ = false;
    SM_LOG_INFO("ralloc master service stopped");
}

bool SmemRallocMasterService::IsRunning() const
{
    return running_;
}

Result SmemRallocMasterService::OnRegister(SmemRallocRpcMsg &msg)
{
    SM_VALIDATE_RETURN(msg.nodeRank != SMEM_RALLOC_INVALID_RANK, "register with invalid rank", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(strlen(msg.nodeIp) != 0, "register with empty ip", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(msg.nodePort != 0, "register with zero port", SM_INVALID_PARAM);

    SmemRallocRpcEndpoint ep{};
    ep.rankId = msg.nodeRank;
    ep.port = static_cast<uint16_t>(msg.nodePort);
    (void)strncpy(ep.ip, msg.nodeIp, sizeof(ep.ip) - 1);

    {
        std::lock_guard<std::mutex> guard(mutex_);
        /* overwrite accounting with the authoritative value reported by the node itself */
        Candidate candidate{};
        candidate.ep = ep;
        candidate.committedBytes = msg.size;
        candidate.deviceCommittedBytes = msg.deviceCommittedBytes;
        auto it = candidates_.find(msg.nodeRank);
        if (it != candidates_.end()) {
            it->second = candidate;
        } else {
            candidates_.emplace(msg.nodeRank, candidate);
        }
    }

    SM_LOG_INFO("candidate registered, rank: " << msg.nodeRank << " endpoint: " << msg.nodeIp << ":"
                                                << msg.nodePort << " committed: " << msg.size
                                                << " deviceCommitted: " << msg.deviceCommittedBytes);
    return SM_OK;
}

Result SmemRallocMasterService::OnPlacement(SmemRallocRpcMsg &msg)
{
    SM_VALIDATE_RETURN(msg.size != 0, "placement size is 0", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(msg.memType == SMEM_RALLOC_MEM_TYPE_HOST || msg.memType == SMEM_RALLOC_MEM_TYPE_DEVICE,
        "placement with invalid mem type", SM_INVALID_PARAM);

    {
        std::lock_guard<std::mutex> guard(mutex_);
        /* pick the least committed candidate of the requested media (last reported value), never
         * place on the requester itself; accounting is overwrite-style, no optimistic add here */
        const bool deviceMedia = msg.memType == SMEM_RALLOC_MEM_TYPE_DEVICE;
        uint32_t chosen = SMEM_RALLOC_INVALID_RANK;
        uint64_t chosenLoad = UINT64_MAX;
        for (auto &it : candidates_) {
            if (it.first == msg.reqRank) {
                continue;
            }
            auto load = deviceMedia ? it.second.deviceCommittedBytes : it.second.committedBytes;
            if (load < chosenLoad) {
                chosen = it.first;
                chosenLoad = load;
            }
        }
        if (chosen == SMEM_RALLOC_INVALID_RANK) {
            SM_LOG_ERROR("no candidate for placement, requester rank: " << msg.reqRank);
            return SM_OBJECT_NOT_EXISTS;
        }

        auto it = candidates_.find(chosen);
        msg.nodeRank = it->second.ep.rankId;
        msg.nodePort = it->second.ep.port;
        (void)memset(msg.nodeIp, 0, sizeof(msg.nodeIp));
        (void)strncpy(msg.nodeIp, it->second.ep.ip, sizeof(msg.nodeIp) - 1);
    }

    SM_LOG_INFO("placement granted, requester: " << msg.reqRank << " memType: " << msg.memType
                                                 << " size: " << msg.size << " -> rank: "
                                                 << msg.nodeRank << " endpoint: " << msg.nodeIp << ":"
                                                 << msg.nodePort);
    return SM_OK;
}
} // namespace smem
} // namespace ock
