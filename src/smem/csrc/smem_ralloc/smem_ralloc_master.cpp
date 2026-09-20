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

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "smem_logger.h"
#include "smem_ralloc_rpc.h"

namespace ock {
namespace smem {
/* a candidate that has not re-registered for this long is considered dead (reporter default
 * interval is 30s, 3 missed periods = death); false positives self-heal on next REGISTER */
constexpr uint32_t SMEMRA_CANDIDATE_STALE_SEC = 90U;

/* backstop lifetime of an in-flight grant: a grant that landed is confirmed by the poked
 * REGISTER within a few seconds (2s poke throttle + transit), so an entry surviving this
 * long is a grant whose executor-side extend failed (e.g. window guard) or whose report
 * was lost; drop it instead of pinning the candidate load forever */
constexpr uint32_t SMEMRA_INFLIGHT_GRANT_TTL_SEC = 15U;

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
            self.lastSeen = std::chrono::steady_clock::now();
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

uint64_t SmemRallocMasterService::SumInflight(const std::vector<InflightGrant> &inflight, bool deviceMedia)
{
    uint64_t sum = 0;
    for (const auto &grant : inflight) {
        if (grant.deviceMedia == deviceMedia) {
            sum += grant.size;
        }
    }
    return sum;
}

void SmemRallocMasterService::ShrinkInflight(std::vector<InflightGrant> &inflight, bool deviceMedia,
                                             uint64_t keepSum)
{
    auto sum = SumInflight(inflight, deviceMedia);
    for (auto grant = inflight.begin(); grant != inflight.end() && sum > keepSum;) {
        if (grant->deviceMedia != deviceMedia) {
            ++grant;
            continue;
        }
        sum -= grant->size;
        grant = inflight.erase(grant);
    }
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
        auto it = candidates_.find(msg.nodeRank);
        Candidate candidate{};
        candidate.ep = ep;
        candidate.committedBytes = msg.size;
        candidate.deviceCommittedBytes = msg.deviceCommittedBytes;
        candidate.lastSeen = std::chrono::steady_clock::now();
        if (it != candidates_.end()) {
            /* reconcile the optimistic ledger: growth of the report confirms the oldest
             * grants have landed on the FAR, drop them; grants still inside the executor
             * keep their reservation, a shrinking report (pool reaped) keeps them all */
            candidate.inflight = std::move(it->second.inflight);
            const uint64_t hostInflight = SumInflight(candidate.inflight, false);
            const uint64_t hostConfirmed = msg.size > it->second.committedBytes
                                               ? msg.size - it->second.committedBytes : 0U;
            ShrinkInflight(candidate.inflight, false, hostInflight - std::min(hostConfirmed, hostInflight));
            const uint64_t devInflight = SumInflight(candidate.inflight, true);
            const uint64_t devConfirmed = msg.deviceCommittedBytes > it->second.deviceCommittedBytes
                                              ? msg.deviceCommittedBytes - it->second.deviceCommittedBytes : 0U;
            ShrinkInflight(candidate.inflight, true, devInflight - std::min(devConfirmed, devInflight));
            it->second = std::move(candidate);
        } else {
            candidates_.emplace(msg.nodeRank, std::move(candidate));
        }
    }

    SM_LOG_DEBUG("candidate registered, rank: " << msg.nodeRank << " endpoint: " << msg.nodeIp << ":"
                                                 << msg.nodePort << " committed: " << msg.size
                                                 << " deviceCommitted: " << msg.deviceCommittedBytes);
    return SM_OK;
}

Result SmemRallocMasterService::OnPlacement(SmemRallocRpcMsg &msg)
{
    SM_VALIDATE_RETURN(msg.size != 0, "placement size is 0", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(msg.memType == SMEM_RALLOC_MEM_TYPE_HOST || msg.memType == SMEM_RALLOC_MEM_TYPE_DEVICE,
        "placement with invalid mem type", SM_INVALID_PARAM);

    uint64_t chosenLoad = UINT64_MAX;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        /* prune candidates whose reporter went silent (crashed node): placement must never
         * pick a node nobody has heard from for 3 report periods */
        const auto now = std::chrono::steady_clock::now();
        for (auto it = candidates_.begin(); it != candidates_.end();) {
            if (now - it->second.lastSeen > std::chrono::seconds(SMEMRA_CANDIDATE_STALE_SEC)) {
                SM_LOG_WARN("prune stale candidate, rank: " << it->first << " endpoint: " << it->second.ep.ip
                                                            << ":" << it->second.ep.port);
                it = candidates_.erase(it);
            } else {
                ++it;
            }
        }
        /* pick the least loaded candidate of the requested media, never place on the
         * requester itself; load = last reported committed bytes plus grants issued since
         * (optimistic in-flight add, reconciled by the next REGISTER): concurrent
         * placements no longer stack on a stale view, and the map-order tie-break only
         * decides genuinely equal loads */
        const bool deviceMedia = msg.memType == SMEM_RALLOC_MEM_TYPE_DEVICE;
        const uint64_t window = deviceMedia ? msg.maxHbmSize : msg.maxDramSize;
        uint32_t chosen = SMEM_RALLOC_INVALID_RANK;
        uint32_t alive = 0;
        for (auto &it : candidates_) {
            if (it.first == msg.reqRank) {
                continue;
            }
            ++alive;
            /* expire grants that never landed (executor failure / lost report) */
            for (auto grant = it.second.inflight.begin(); grant != it.second.inflight.end();) {
                if (now - grant->at > std::chrono::seconds(SMEMRA_INFLIGHT_GRANT_TTL_SEC)) {
                    grant = it.second.inflight.erase(grant);
                } else {
                    ++grant;
                }
            }
            auto load = (deviceMedia ? it.second.deviceCommittedBytes : it.second.committedBytes) +
                        SumInflight(it.second.inflight, deviceMedia);
            /* capacity filter mirroring the executor window guard: a full contributor is
             * skipped instead of granted and failed; window == 0 (requester did not fill
             * the field, old binary) disables the filter */
            if (window != 0 && load + msg.size > window) {
                SM_LOG_DEBUG("candidate rank: " << it.first << " filtered by window, load: " << load
                                                << " size: " << msg.size << " window: " << window);
                continue;
            }
            if (load < chosenLoad) {
                chosen = it.first;
                chosenLoad = load;
            }
        }
        if (chosen == SMEM_RALLOC_INVALID_RANK) {
            if (alive != 0) {
                SM_LOG_ERROR("no candidate with spare capacity for placement, requester rank: " << msg.reqRank
                             << " size: " << msg.size << " window: " << window
                             << " alive candidates: " << alive);
            } else {
                SM_LOG_ERROR("no candidate for placement, requester rank: " << msg.reqRank);
            }
            return SM_OBJECT_NOT_EXISTS;
        }

        auto it = candidates_.find(chosen);
        msg.nodeRank = it->second.ep.rankId;
        msg.nodePort = it->second.ep.port;
        (void)memset(msg.nodeIp, 0, sizeof(msg.nodeIp));
        (void)strncpy(msg.nodeIp, it->second.ep.ip, sizeof(msg.nodeIp) - 1);
        /* reserve the grant optimistically so placements granted before the next report
         * see it in the load above */
        it->second.inflight.push_back(InflightGrant{msg.size, deviceMedia, now});
    }

    SM_LOG_INFO("placement granted, requester: " << msg.reqRank << " memType: " << msg.memType
                                                 << " size: " << msg.size << " load: " << chosenLoad
                                                 << " -> rank: " << msg.nodeRank << " endpoint: "
                                                 << msg.nodeIp << ":" << msg.nodePort);
    return SM_OK;
}

void SmemRallocMasterService::OnRankDown(uint32_t rank)
{
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto erased = candidates_.erase(rank);
        SM_LOG_WARN("candidate rank-down, rank: " << rank << " existed: " << erased);
    }
    /* idempotent: an absent rank is a no-op (e.g. NEAR-only links also fire rank-down);
     * a wrongly dropped node re-registers on its next periodic report, the table
     * self-heals within one report interval */
}
} // namespace smem
} // namespace ock
