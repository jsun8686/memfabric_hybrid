/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
*/
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "hybm_logger.h"
#include "dl_acl_api.h"
#include "dl_hccp_api.h"
#include "fixed_ranks_qp_manager.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {
constexpr int MR_INFO_ACCESS = 7;
constexpr uint32_t QP_VERSION = 1;
constexpr uint32_t SEND_CQ_DEPTH = 8192;
constexpr uint32_t RECV_DQ_DEPTH = 128;
constexpr uint32_t MAX_RECV_SGE = 1;
constexpr uint32_t MAX_RECV_WR = 128;
constexpr uint32_t MAX_SEND_WR = 8192;
constexpr uint32_t CQ_CUSTOM_FLAG = 1;
constexpr int COPY_INFO_SL = 4;

void FixedRanksQpManager::MakeLaneTag(char *buf, size_t bufSize, uint32_t clientRank, uint32_t lane) noexcept
{
    /* derived from the CLIENT's rank so both ends derive the same string (same scheme as
     * HCCL's MakeUniqueConnTag); lane 0 stays on the legacy empty-tag primary connection */
    snprintf(buf, bufSize, "mf_q2_%u_%u", clientRank, lane);
}

FixedRanksQpManager::FixedRanksQpManager(uint32_t deviceId, uint32_t rankId, uint32_t rankCount,
                                         sockaddr_in devNet) noexcept
    : DeviceQpManager(deviceId, rankId, rankCount, devNet, HYBM_ROLE_PEER)
{}

FixedRanksQpManager::~FixedRanksQpManager() noexcept
{
    CloseServices();
    ReleaseQpInfoSpace();
}

int FixedRanksQpManager::SetRemoteRankInfo(const std::unordered_map<uint32_t, ConnectRankInfo> &ranks) noexcept
{
    if (started_.load()) {
        /* dynamic groups (ralloc) import slices/ranks incrementally after startup: merge the new
         * entries (memKeys grow per slice) instead of rejecting, then refresh the device-side
         * QP/MR table. Connections are owned by the build-once worker; a refresh failure is not
         * fatal (QP entries absent yet are re-filled by its own FillQpInfo once established). */
        {
            std::lock_guard<std::mutex> lock(mapsMutex_);
            for (auto it = ranks.begin(); it != ranks.end(); ++it) {
                currentRanksInfo_.insert_or_assign(it->first, it->second);
            }
        }
        DlAclApi::AclrtSetDevice(deviceId_);
        if (FillQpInfo(false) != BM_OK) {
            BM_LOG_WARN("refresh qp info after rank update failed; deferred to build worker");
        }
        MaybeTriggerBuild();
        return BM_OK;
    }

    currentRanksInfo_ = ranks;
    return BM_OK;
}

int FixedRanksQpManager::Startup(void *rdma) noexcept
{
    if (rdma == nullptr) {
        BM_LOG_ERROR("input rdma is null");
        return BM_INVALID_PARAM;
    }

    if (started_.load()) {
        BM_LOG_DEBUG("already started.");
        return BM_OK;
    }

    rdmaHandle_ = rdma;
    qpsPerPeer_ = ResolveQpsPerPeer();
    if (qpsPerPeer_ > 1U) {
        BM_LOG_INFO("multi-QP enabled: " << qpsPerPeer_ << " QP lanes per peer (tag multiplexing)");
    }
    /* Startup may run on a group-callback thread that has never bound a device context
     * (ralloc dynamic join -> ImportEntityExchangeInfo -> Prepare). ReserveQpInfoSpace
     * calls AclrtMalloc, which is thread-context bound; bind this thread first, same
     * as the BuildOnce worker thread. */
    DlAclApi::AclrtSetDevice(deviceId_);
    if (!ReserveQpInfoSpace()) {
        BM_LOG_ERROR("reserve qp info space failed.");
        return BM_ERROR;
    }

    /* dynamic groups (ralloc) may have fewer members than the world size: the qp table is sized
     * by rankCount_ but only filled for the ranks present, so only reject an overflow here */
    if (currentRanksInfo_.size() > rankCount_) {
        BM_LOG_ERROR("set rank count = " << currentRanksInfo_.size() << ", but rank_size = " << rankCount_);
        return BM_INVALID_PARAM;
    }

    for (auto it = currentRanksInfo_.begin(); it != currentRanksInfo_.end(); ++it) {
        if (it->first >= rankCount_) {
            BM_LOG_ERROR("input options of nics contains rankId:" << it->first << ", rank count: " << rankCount_);
            return BM_INVALID_PARAM;
        }
    }

    auto ret = StartServerSide();
    if (ret != BM_OK) {
        BM_LOG_ERROR("start server side failed: " << ret);
        return ret;
    }

    started_.store(true);
    /* static groups have every member in the table BEFORE Startup ran (the full-assignment
     * SetRemoteRankInfo could not trigger -- started_ was still false), and no further member
     * event may ever arrive: self-check the group-full condition here */
    MaybeTriggerBuild();
    return BM_OK;
}

void FixedRanksQpManager::Shutdown() noexcept
{
    CloseServices();
    ReleaseQpInfoSpace();
}

int FixedRanksQpManager::WaitingConnectionReady() noexcept
{
    if (!started_.load()) {
        BM_LOG_ERROR("waiting connection ready before startup.");
        return BM_ERROR;
    }

    size_t memberCount;
    {
        std::lock_guard<std::mutex> lock(mapsMutex_);
        memberCount = currentRanksInfo_.size();
    }
    if (memberCount < rankCount_) {
        /* group not full yet: pass through immediately. This gate runs on the group-event
         * thread inside hybm_import (before the join round's barrier) while peers are still
         * joining -- blocking here froze the group engine's serial event queue (the FSDP
         * join-round deadlock). The full-group build is triggered by the last joiner's event;
         * remote IO is impossible before that (MRs of absent peers have not arrived) */
        BM_LOG_INFO("group not full (" << memberCount << "/" << rankCount_ << "); connections deferred");
        return BM_OK;
    }

    /* group is full: wait for the one-shot build to converge (budget: 3 rounds are bounded
     * by the short per-round waits; a healthy full group converges in seconds) */
    std::unique_lock<std::mutex> lock(stateMutex_);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(35);
    while (buildState_ != BuildState::Done && buildState_ != BuildState::Failed) {
        if (stateCv_.wait_until(lock, deadline) == std::cv_status::timeout) {
            break;
        }
        if (!started_.load()) {
            /* Shutdown woke us up */
            return BM_ERROR;
        }
    }
    if (buildState_ == BuildState::Done) {
        BM_LOG_INFO("build-once connections ready.");
        return BM_OK;
    }
    BM_LOG_ERROR("build-once connections not ready (state: "
                 << (buildState_ == BuildState::Failed ? "failed" : "timeout") << ").");
    return BM_ERROR;
}

const void *FixedRanksQpManager::GetQpInfoAddress() const noexcept
{
    return qpInfo_;
}

UserQpInfo *FixedRanksQpManager::GetQpHandleWithRankId(uint32_t rankId) noexcept
{
    BM_LOG_ERROR("FixedRanksQpManager can't get qp!");
    return nullptr;
}

void FixedRanksQpManager::PutQpHandle(UserQpInfo *qp) const noexcept
{
    return;
}

bool FixedRanksQpManager::ReserveQpInfoSpace() noexcept
{
    if (qpInfo_ != nullptr) {
        return true;
    }

    void *ptr = nullptr;
    auto oneQpSize = 2U * (sizeof(AiQpRMAWQ) + sizeof(AiQpRMACQ)) + sizeof(RdmaMemRegionInfo) * MR_SLOTS_PER_RANK;
    qpInfoSize_ = sizeof(AiQpRMAQueueInfo) + oneQpSize * rankCount_ * qpsPerPeer_;
    auto ret = DlAclApi::AclrtMalloc(&ptr, qpInfoSize_, 0);
    if (ret != 0) {
        BM_LOG_ERROR("allocate device size: " << qpInfoSize_ << ", failed: " << ret);
        return false;
    }

    qpInfo_ = (AiQpRMAQueueInfo *)ptr;
    return true;
}

void FixedRanksQpManager::ReleaseQpInfoSpace() noexcept
{
    if (qpInfo_ != nullptr) {
        DlAclApi::AclrtFree(qpInfo_);
        qpInfo_ = nullptr;
    }
}

int FixedRanksQpManager::StartServerSide() noexcept
{
    if (rankId_ + 1U == rankCount_) {
        return BM_OK;
    }

    auto ret = CreateServerSocket();
    if (ret != BM_OK) {
        BM_LOG_ERROR("create server socket failed: " << ret);
        return ret;
    }

    ret = GenerateWhiteList();
    if (ret != 0) {
        BM_LOG_ERROR("generate white list failed: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }

    return BM_OK;
}

void FixedRanksQpManager::MaybeTriggerBuild() noexcept
{
    if (!started_.load()) {
        return;
    }

    size_t memberCount;
    size_t channelCount;
    {
        std::lock_guard<std::mutex> lock(mapsMutex_);
        memberCount = currentRanksInfo_.size();
        channelCount = clientConnections_.size() + serverConnections_.size();
    }
    if (memberCount != rankCount_) {
        return; /* group not full: the last joiner's event will trigger this again */
    }

    std::lock_guard<std::mutex> lock(stateMutex_);
    if (buildState_ == BuildState::Building) {
        return;
    }
    if (buildState_ == BuildState::Failed) {
        /* storm guard: after a failed build only retry past the cool-down */
        if (std::chrono::steady_clock::now() - lastBuildFailSteady_ < std::chrono::seconds(5)) {
            return;
        }
    } else if (buildState_ == BuildState::Done && channelCount + 1U == rankCount_) {
        return; /* established and complete: nothing to (re)build */
    }
    if (buildThread_ != nullptr && buildThread_->joinable()) {
        /* state already left Building, so the worker body has returned; reclaim it (returns
         * immediately) before replacing the thread object */
        buildThread_->join();
    }

    BM_LOG_INFO("group full (" << memberCount << "/" << rankCount_ << "); starting build-once worker");
    buildState_ = BuildState::Building;
    buildThread_ = std::make_shared<std::thread>([this]() { BuildOnce(); });
}

void FixedRanksQpManager::BuildOnce() noexcept
{
    DlAclApi::AclrtSetDevice(deviceId_);
    constexpr int MAX_ROUNDS = 3;
    for (int round = 1; round <= MAX_ROUNDS; round++) {
        if (!started_.load()) {
            /* Shutdown in progress: bail out (the tail marks Failed and wakes the gate) */
            break;
        }
        if (rankId_ + 1U != rankCount_ && GenerateWhiteList() != BM_OK) {
            /* idempotent re-add: hccp whitelists are persistent; failure is ordinary "retry" */
            BM_LOG_WARN("build-once round " << round << ": generate white list failed; retrying");
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        if (round == 1) {
            /* warm-up: let the local whitelist entry take effect AND let the peers' own
             * (near-simultaneous) full-group writes land before the first connect */
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!LaunchClientConnects()) {
            CleanupRound();
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        auto sret = WaitConnectionsReady(serverConnections_);
        if (sret == BM_OK) {
            sret = CreateQpWaitingReady(serverConnections_);
        }
        auto cret = BM_OK;
        if (sret == BM_OK && !clientConnections_.empty()) {
            cret = WaitConnectionsReady(clientConnections_);
            if (cret == BM_OK) {
                cret = CreateQpWaitingReady(clientConnections_);
            }
        }
        if (sret == BM_OK && cret == BM_OK) {
            std::lock_guard<std::mutex> lock(stateMutex_);
            buildState_ = BuildState::Done;
            stateCv_.notify_all();
            BM_LOG_INFO("build-once: all connections established after round " << round);
            return;
        }

        BM_LOG_WARN("build-once round " << round << " failed (server: " << sret << ", client: " << cret
                                        << "); cleaning up and retrying");
        CleanupRound();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        buildState_ = BuildState::Failed;
        lastBuildFailSteady_ = std::chrono::steady_clock::now();
    }
    stateCv_.notify_all();
    BM_LOG_ERROR("build-once: all rounds failed; deferring to the next member event");
}

bool FixedRanksQpManager::LaunchClientConnects() noexcept
{
    if (rankId_ == 0U) {
        return true; /* nobody is smaller than rank 0 */
    }

    std::vector<HccpSocketConnectInfo> connectInfos;
    std::vector<uint32_t> added;
    {
        std::lock_guard<std::mutex> lock(mapsMutex_);
        for (auto it = currentRanksInfo_.begin(); it != currentRanksInfo_.end(); ++it) {
            if (it->first >= rankId_ || clientConnections_.find(it->first) != clientConnections_.end()) {
                continue; /* client connects to smaller ranks only; skip established channels */
            }

            auto socketHandle = CreateLocalSocket();
            if (socketHandle == nullptr) {
                BM_LOG_WARN("create local socket for rank " << it->first << " failed; retry later");
                return false;
            }
            clientConnections_.emplace(it->first, AiCoreConnChannel{it->second.network.sin_addr, socketHandle});
            HccpSocketConnectInfo connectInfo{};
            connectInfo.handle = socketHandle;
            connectInfo.remoteIp.addr = it->second.network.sin_addr;
            connectInfo.port = it->second.network.sin_port;
            bzero(connectInfo.tag, sizeof(connectInfo.tag));
            connectInfos.emplace_back(connectInfo);
            if (qpsPerPeer_ > 1U) {
                /* extra tagged lanes ride the SAME (ip, port) endpoint and the SAME local handle:
                 * hccp multiplexes connections by tag; we are the client, so the tag derives
                 * from our own rank */
                for (uint32_t lane = 1; lane < qpsPerPeer_; lane++) {
                    HccpSocketConnectInfo extra = connectInfo;
                    MakeLaneTag(extra.tag, sizeof(extra.tag), rankId_, lane);
                    connectInfos.emplace_back(extra);
                }
            }
            added.emplace_back(it->first);
        }
    }

    if (connectInfos.empty()) {
        return true;
    }

    auto ret = DlHccpApi::RaSocketBatchConnect(connectInfos.data(), connectInfos.size());
    if (ret != 0) {
        /* drop only this attempt's half-added entries (the client owns its handles) */
        std::lock_guard<std::mutex> lock(mapsMutex_);
        for (auto rank : added) {
            auto pos = clientConnections_.find(rank);
            if (pos != clientConnections_.end()) {
                (void)DlHccpApi::RaSocketDeinit(pos->second.socketHandle);
                clientConnections_.erase(pos);
            }
        }
        BM_LOG_WARN("batch connect failed: " << ret << "; retrying");
        return false;
    }
    return true;
}

void FixedRanksQpManager::CleanupRound() noexcept
{
    std::vector<HccpSocketCloseInfo> closeInfos;
    {
        std::lock_guard<std::mutex> lock(mapsMutex_);
        for (auto it = clientConnections_.begin(); it != clientConnections_.end();) {
            const bool established = (it->second.socketFd[0] != nullptr && it->second.qpHandle[0] != nullptr &&
                                      it->second.qpStatus[0] == 1);
            if (!established) {
                for (uint32_t lane = 0; lane < qpsPerPeer_; lane++) {
                    if (it->second.qpHandle[lane] != nullptr) {
                        (void)DlHccpApi::RaQpDestroy(it->second.qpHandle[lane]);
                        it->second.qpHandle[lane] = nullptr;
                        it->second.qpStatus[lane] = -1;
                    }
                    if (it->second.socketFd[lane] != nullptr) {
                        HccpSocketCloseInfo closeInfo{};
                        closeInfo.handle = it->second.socketHandle;
                        closeInfo.fd = it->second.socketFd[lane];
                        closeInfo.linger = 0;
                        closeInfos.emplace_back(closeInfo);
                        it->second.socketFd[lane] = nullptr;
                    }
                }
                if (it->second.socketHandle != nullptr) {
                    (void)DlHccpApi::RaSocketDeinit(it->second.socketHandle);
                }
                it = clientConnections_.erase(it);
            } else {
                ++it;
            }
        }
        /* server entries without a delivered fd: drop (the peer's client retries the connect);
         * entries WITH a fd survive even if their QP is not ready yet -- the next round's
         * WaitConnectionsReady counts them as settled and CreateQpWaitingReady rebuilds the QP */
        for (auto it = serverConnections_.begin(); it != serverConnections_.end();) {
            if (it->second.socketFd[0] == nullptr) {
                it = serverConnections_.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (!closeInfos.empty()) {
        auto ret = DlHccpApi::RaSocketBatchClose(closeInfos.data(), closeInfos.size());
        if (ret != 0) {
            BM_LOG_INFO("build-once cleanup: batch close sockets return: " << ret);
        }
    }
}

int FixedRanksQpManager::GenerateWhiteList() noexcept
{
    std::vector<HccpSocketWhiteListInfo> whitelist;
    {
        /* runs on the BuildOnce thread concurrently with group-event merges: snapshot the
         * member table and extend the server-channel map under the structure lock. The
         * hccp RaSocketWhiteListAdd inside is idempotent (whitelists are persistent) */
        std::lock_guard<std::mutex> lock(mapsMutex_);
        for (auto it = currentRanksInfo_.begin(); it != currentRanksInfo_.end(); ++it) {
            if (it->first <= rankId_) {
                continue; // small id as server, large id as client
            }
            HccpSocketWhiteListInfo info{};
            info.remoteIp.addr = it->second.network.sin_addr;
            info.connLimit = rankCount_;
            bzero(info.tag, sizeof(info.tag));
            whitelist.emplace_back(info);
            serverConnections_.emplace(it->first, AiCoreConnChannel{info.remoteIp.addr, serverSocketHandle_});
            if (qpsPerPeer_ > 1U) {
                /* whitelist the peer's tagged lanes up front, before any client connects (same
                 * ordering as HCCL's ConstructWhiteList); the peer is the connecting client, so
                 * the tag derives from the remote rank */
                for (uint32_t lane = 1; lane < qpsPerPeer_; lane++) {
                    HccpSocketWhiteListInfo extra{};
                    extra.remoteIp.addr = it->second.network.sin_addr;
                    extra.connLimit = rankCount_;
                    MakeLaneTag(extra.tag, sizeof(extra.tag), it->first, lane);
                    whitelist.emplace_back(extra);
                    BM_LOG_INFO("multi-QP server: lane " << lane << " whitelist rank " << it->first
                                                              << " tag '" << extra.tag << "'");
                }
            }
        }
    } /* structure lock released before the hccp call below */

    if (whitelist.empty()) {
        return BM_OK;
    }

    auto ret = DlHccpApi::RaSocketWhiteListAdd(serverSocketHandle_, whitelist.data(), whitelist.size());
    if (ret != 0) {
        BM_LOG_ERROR("socket handle add white list failed: " << ret);
        return BM_ERROR;
    }

    return BM_OK;
}

int FixedRanksQpManager::CheckReadyConnection(std::unordered_map<uint32_t, AiCoreConnChannel> &connections,
                                              const std::unordered_map<in_addr_t, uint32_t> addr2index,
                                              const HccpSocketInfo &socketInfo) noexcept
{
    auto &addr = socketInfo.remoteIp.addr;
    auto socketInfoPos = addr2index.find(addr.s_addr);
    if (socketInfoPos == addr2index.end()) {
        BM_LOG_ERROR("socket ip(" << DescribeIPv4(addr) << ") should exist in address mapping.");
        return BM_DL_FUNCTION_FAILED;
    }

    auto rankId = socketInfoPos->second;
    auto pos = connections.find(rankId);
    if (pos == connections.end()) {
        BM_LOG_ERROR("socket with rank(" << rankId << ") should exist in connections.");
        return BM_DL_FUNCTION_FAILED;
    }

    if (pos->second.socketFd[0] != nullptr) {
        if (qpsPerPeer_ > 1U) {
            /* a tagged lane connection surfaced in the empty-tag primary polling; harmless --
             * lane fds are fetched by their own tag-scoped queries in CreateQpWaitingReady */
            BM_LOG_INFO("multi-QP: extra accept from rank " << rankId << " surfaced in primary polling, fd: "
                          << socketInfo.fd << " tag: '" << socketInfo.tag << "'");
            return BM_OK;
        }
        BM_LOG_ERROR("socket ip(" << DescribeIPv4(addr) << ") already get socket fd.");
        return BM_DL_FUNCTION_FAILED;
    }

    if (pos->second.socketHandle != socketInfo.handle) {
        BM_LOG_ERROR("socket ip(" << DescribeIPv4(addr) << ") socket handle not match.");
        return BM_DL_FUNCTION_FAILED;
    }

    pos->second.socketFd[0] = socketInfo.fd;
    BM_LOG_INFO("connect to (" << rankId << ") ready.");
    return BM_OK;
}

int FixedRanksQpManager::WaitConnectionsReady(std::unordered_map<uint32_t, AiCoreConnChannel> &connections) noexcept
{
    /* pre-settled entries count from the start: BuildOnce re-walks the whole map on every
     * round (re-triggered builds keep established channels), and a settled entry never adds
     * to the count inside the loop -- without this base it would spin to the timeout */
    uint32_t totalSuccessCount = 0;
    for (auto it = connections.begin(); it != connections.end(); ++it) {
        if (it->second.socketFd[0] != nullptr) {
            totalSuccessCount++;
        }
    }
    auto start = std::chrono::steady_clock::now();
    auto timeout = start + std::chrono::seconds(15);
    while (totalSuccessCount < connections.size()) {
        if (!started_.load()) {
            /* Shutdown in progress: stop waiting so CloseServices joins fast */
            return BM_ERROR;
        }
        if (std::chrono::steady_clock::now() >= timeout) {
            BM_LOG_ERROR("waiting connection ready timeout.");
            return BM_ERROR;
        }

        uint32_t successCount = 0;
        std::vector<HccpSocketInfo> socketInfos;
        std::unordered_map<in_addr_t, uint32_t> addr2index;
        for (auto it = connections.begin(); it != connections.end(); ++it) {
            if (it->second.socketFd[0] != nullptr) {
                continue;
            }

            HccpSocketInfo info{};
            info.handle = it->second.socketHandle;
            info.fd = nullptr;
            info.remoteIp.addr = it->second.remoteIp;
            info.status = 0;
            bzero(info.tag, sizeof(info.tag));
            socketInfos.push_back(info);
            addr2index.emplace(it->second.remoteIp.s_addr, it->first);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        auto role = (&connections == &clientConnections_) ? 1 : 0;
        auto ret = DlHccpApi::RaGetSockets(role, socketInfos.data(), socketInfos.size(), successCount);
        if (ret != 0) {
            if (ret == 128203) {
                /* hccp reports "no completed connection to hand out yet" as this code: in the
                 * build-once model that is ordinary "no progress this poll", not an error */
                successCount = 0;
            } else {
                BM_LOG_ERROR("role(" << role << ") side get sockets failed: " << ret);
                return BM_DL_FUNCTION_FAILED;
            }
        }

        for (auto i = 0U; i < successCount; i++) {
            ret = CheckReadyConnection(connections, addr2index, socketInfos[i]);
            if (ret != BM_OK) {
                return ret;
            }
        }

        totalSuccessCount += successCount;
    }

    return BM_OK;
}

int FixedRanksQpManager::CreateQpWaitingReady(std::unordered_map<uint32_t, AiCoreConnChannel> &connections) noexcept
{
    const bool clientSide = (&connections == &clientConnections_);
    /* lane 0: legacy primary connections, one QP per rank. Skip lanes already established
     * (re-triggered builds keep working channels) and destroy half-broken leftovers on the
     * spot: a connect-failed / not-ready QP poisons every later poll (observed as endless
     * get-QP-status failures after a connect failure), so it must not survive the round */
    for (auto it = connections.begin(); it != connections.end(); ++it) {
        if (it->second.qpHandle[0] != nullptr) {
            if (it->second.qpStatus[0] == 1) {
                continue;
            }
            (void)DlHccpApi::RaQpDestroy(it->second.qpHandle[0]);
            it->second.qpHandle[0] = nullptr;
            it->second.qpStatus[0] = -1;
        }
        auto ret = CreateOneQp(it->second, 0);
        if (ret != 0) {
            BM_LOG_ERROR("create QP  to " << it->first << " failed: " << ret);
            it->second.qpHandle[0] = nullptr; /* a failed create must not leave a stale handle */
            return BM_DL_FUNCTION_FAILED;
        }

        ret = DlHccpApi::RaQpConnectAsync(it->second.qpHandle[0], it->second.socketFd[0]);
        if (ret != 0) {
            BM_LOG_ERROR("connect AI QP to " << it->first << " failed: " << ret);
            (void)DlHccpApi::RaQpDestroy(it->second.qpHandle[0]);
            it->second.qpHandle[0] = nullptr;
            it->second.qpStatus[0] = -1;
            return BM_DL_FUNCTION_FAILED;
        }
    }

    if (qpsPerPeer_ > 1U) {
        /* extra tagged lanes: fetch each lane fd by its tag-scoped query, create its QP and
         * connect async. All-or-nothing: a dead lane would leave a hole in the device table
         * that kernels post into, so any failure aborts the startup. */
        auto laneBase = std::chrono::steady_clock::now();
        for (auto it = connections.begin(); it != connections.end(); ++it) {
            auto &channel = it->second;
            const uint32_t clientRank = clientSide ? rankId_ : it->first;
            for (uint32_t lane = 1; lane < qpsPerPeer_; lane++) {
                char tag[64];
                MakeLaneTag(tag, sizeof(tag), clientRank, lane);
                void *laneFd = nullptr;
                auto deadline = laneBase + std::chrono::seconds(15);
                while (std::chrono::steady_clock::now() < deadline) {
                    HccpSocketInfo info{};
                    info.handle = channel.socketHandle;
                    info.fd = nullptr;
                    info.remoteIp.addr = channel.remoteIp;
                    info.status = 0;
                    snprintf(info.tag, sizeof(info.tag), "%s", tag);
                    uint32_t cnt = 0;
                    auto gret = DlHccpApi::RaGetSockets(clientSide ? 1U : 0U, &info, 1, cnt);
                    if (gret == 0 && cnt > 0 && info.status == 1 && info.fd != nullptr &&
                        info.fd != channel.socketFd[0]) {
                        laneFd = info.fd;
                        break;
                    }
                    if (gret == 0 && cnt > 0 && info.status == 2) {
                        BM_LOG_ERROR("multi-QP lane " << lane << " to rank " << it->first
                                                        << " connect rejected/timeout (tag '" << tag << "')");
                        return BM_DL_FUNCTION_FAILED;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                if (laneFd == nullptr) {
                    BM_LOG_ERROR("multi-QP lane " << lane << " to rank " << it->first
                                                    << " no fd within deadline (tag '" << tag << "')");
                    return BM_DL_FUNCTION_FAILED;
                }
                channel.socketFd[lane] = laneFd;
                auto ret = CreateOneQp(channel, lane);
                if (ret != 0) {
                    BM_LOG_ERROR("create lane " << lane << " QP to " << it->first << " failed: " << ret);
                    return BM_DL_FUNCTION_FAILED;
                }
                ret = DlHccpApi::RaQpConnectAsync(channel.qpHandle[lane], laneFd);
                if (ret != 0) {
                    BM_LOG_ERROR("connect lane " << lane << " QP to " << it->first << " failed: " << ret);
                    return BM_DL_FUNCTION_FAILED;
                }
                BM_LOG_INFO("multi-QP " << (clientSide ? "client" : "server") << " rank " << it->first
                                            << " lane " << lane << " fd " << laneFd << " tag '" << tag << "'");
            }
        }
    }

    auto start = std::chrono::steady_clock::now();
    auto timeout = start + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < timeout) {
        if (!started_.load()) {
            /* Shutdown in progress: stop polling so CloseServices joins fast */
            return BM_ERROR;
        }
        int connectingCount = 0;
        for (auto it = connections.begin(); it != connections.end(); ++it) {
            for (uint32_t lane = 0; lane < qpsPerPeer_; lane++) {
                int status = 0;
                auto ret = DlHccpApi::RaGetQpStatus(it->second.qpHandle[lane], status);
                if (ret != 0) {
                    BM_LOG_ERROR("get AI QP status to " << it->first << " lane " << lane << " failed: " << ret);
                    return BM_DL_FUNCTION_FAILED;
                }
                it->second.qpStatus[lane] = status;
                if (status != 1) {
                    connectingCount++;
                }
            }
        }
        if (connectingCount == 0) {
            return FillQpInfo();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return BM_TIMEOUT;
}

int FixedRanksQpManager::CreateOneQp(AiCoreConnChannel &channel, uint32_t lane) noexcept
{
    HccpQpExtAttrs attr{};
    attr.qpMode = NETWORK_OFFLINE;
    attr.version = QP_VERSION;
    attr.cqAttr.sendCqDepth = SEND_CQ_DEPTH;
    attr.cqAttr.recvCqDepth = RECV_DQ_DEPTH;
    attr.qp_attr.cap.max_recv_sge = MAX_RECV_SGE;
    attr.qp_attr.cap.max_recv_wr = MAX_RECV_WR;
    attr.qp_attr.cap.max_recv_sge = MAX_RECV_SGE;
    attr.qp_attr.qp_type = IBV_QPT_RC;
    attr.qp_attr.cap.max_send_wr = MAX_SEND_WR;
    attr.data_plane_flag.bs.cq_cstm = CQ_CUSTOM_FLAG;
    auto ret = DlHccpApi::RaQpAiCreate(rdmaHandle_, attr, channel.aiQpInfo[lane], channel.qpHandle[lane]);
    return ret;
}

int FixedRanksQpManager::FillQpInfo(bool strict) noexcept
{
    if (qpInfoSize_ == 0U || qpInfo_ == nullptr) {
        /* reachable when a rank update arrives while the table is not reserved yet (e.g. tests
         * or a startup race): the zero-size buffer below would hand out a null data pointer.
         * The SetRemoteRankInfo refresh path treats this as a benign skip (warn + deferred) */
        BM_LOG_WARN("fill qp info skipped: qp table not reserved yet");
        return BM_ERROR;
    }
    std::vector<uint8_t> qpInfoBuffer(qpInfoSize_);
    auto copyInfo = (AiQpRMAQueueInfo *)(void *)qpInfoBuffer.data();
    /* count = QP lanes per connection; the sq/rq/scq/rcq arrays are indexed
     * rank * qpsPerPeer_ + lane, the mr array stays per-rank (rkey is lane-independent) */
    copyInfo->count = qpsPerPeer_;
    const auto qpSlotCount = rankCount_ * qpsPerPeer_;
    copyInfo->sq = (AiQpRMAWQ *)(void *)(copyInfo + 1);
    copyInfo->rq = (AiQpRMAWQ *)(void *)(copyInfo->sq + qpSlotCount);
    copyInfo->scq = (AiQpRMACQ *)(void *)(copyInfo->rq + qpSlotCount);
    copyInfo->rcq = (AiQpRMACQ *)(void *)(copyInfo->scq + qpSlotCount);
    copyInfo->mr = (RdmaMemRegionInfo *)(void *)(copyInfo->rcq + qpSlotCount);
    {
        /* snapshot under the structure lock: the group-event thread refreshes here
         * concurrently with the BuildOnce thread's structural edits; a channel the worker
         * has not reported ready yet reads as stale zeros -- harmless, the worker refreshes
         * the table again once its QPs become ready */
        std::lock_guard<std::mutex> lock(mapsMutex_);
        for (auto it = currentRanksInfo_.begin(); it != currentRanksInfo_.end(); ++it) {
            auto &map = it->second.memoryMap;
            if (map.empty()) {
                continue;
            }
            /* fill up to MR_SLOTS_PER_RANK slots per rank (map is ordered by address, descending):
             * slot 0 holds the highest-address block; unused slots stay zero (addr == 0) */
            auto *mrSlots = copyInfo->mr + it->first * MR_SLOTS_PER_RANK;
            uint32_t filled = 0;
            for (auto mrIt = map.begin(); mrIt != map.end() && filled < MR_SLOTS_PER_RANK; ++mrIt, ++filled) {
                auto &slot = mrSlots[filled];
                slot.size = mrIt->second.size;
                slot.addr = mrIt->second.address;
                slot.lkey = mrIt->second.lkey;
                slot.rkey = mrIt->second.rkey;
                /* device-dma base the MR was registered under (host-dram iova; equals address for hbm):
                 * the device-side WQE/SGE addresses must be translated into this range */
                slot.regAddress = mrIt->second.regAddress;
            }
            if (map.size() > MR_SLOTS_PER_RANK) {
                BM_LOG_WARN("rank " << it->first << " has " << map.size()
                                    << " memory regions, only the first " << MR_SLOTS_PER_RANK
                                    << " are visible to device-scheduled kernels");
            }
            if (it->first == rankId_) {
                continue;
            }

            std::unordered_map<uint32_t, AiCoreConnChannel> *connections;
            if (it->first < rankId_) {
                connections = &clientConnections_;
            } else {
                connections = &serverConnections_;
            }

            auto pos = connections->find(it->first);
            if (pos == connections->end()) {
                if (strict) {
                    BM_LOG_ERROR("missing for remote: " << it->first);
                    return BM_ERROR;
                }
                /* refresh path: the member's MR slots above are already filled; its QP slots
                 * stay stale zeros until the build-once worker establishes the connection */
                BM_LOG_INFO("qp entries not established for rank " << it->first
                            << " yet (refresh deferred)");
                continue;
            }

            for (uint32_t lane = 0; lane < qpsPerPeer_; lane++) {
                const auto &laneInfo = pos->second.aiQpInfo[lane].data_plane_info;
                CopyAiWQInfo(copyInfo->sq[it->first * qpsPerPeer_ + lane], laneInfo.sq, DBMode::HW_DB, COPY_INFO_SL);
                CopyAiWQInfo(copyInfo->rq[it->first * qpsPerPeer_ + lane], laneInfo.rq, DBMode::SW_DB, COPY_INFO_SL);
                CopyAiCQInfo(copyInfo->scq[it->first * qpsPerPeer_ + lane], laneInfo.scq, DBMode::HW_DB);
                CopyAiCQInfo(copyInfo->rcq[it->first * qpsPerPeer_ + lane], laneInfo.rcq, DBMode::SW_DB);
            }
        }
    } /* structure lock released; the device copy below runs lock-free */

    auto pointer = (size_t)(void *)(qpInfo_);
    pointer += sizeof(AiQpRMAQueueInfo);
    copyInfo->sq = (AiQpRMAWQ *)(void *)(pointer);

    pointer += sizeof(AiQpRMAWQ) * rankCount_ * qpsPerPeer_;
    copyInfo->rq = (AiQpRMAWQ *)(void *)(pointer);

    pointer += sizeof(AiQpRMAWQ) * rankCount_ * qpsPerPeer_;
    copyInfo->scq = (AiQpRMACQ *)(void *)(pointer);

    pointer += sizeof(AiQpRMACQ) * rankCount_ * qpsPerPeer_;
    copyInfo->rcq = (AiQpRMACQ *)(void *)(pointer);

    pointer += sizeof(AiQpRMACQ) * rankCount_ * qpsPerPeer_;
    copyInfo->mr = (RdmaMemRegionInfo *)(void *)pointer;

    auto ret = DlAclApi::AclrtMemcpy(qpInfo_, qpInfoSize_, copyInfo, qpInfoSize_, ACL_MEMCPY_HOST_TO_DEVICE);
    if (ret != 0) {
        BM_LOG_ERROR("copy qp info to device failed: " << ret);
        return BM_DL_FUNCTION_FAILED;
    }
    BM_LOG_INFO("copy qp info success");

    return BM_OK;
}

void FixedRanksQpManager::CopyAiWQInfo(struct AiQpRMAWQ &dest, const struct ai_data_plane_wq &src, DBMode dbMode,
                                       uint32_t sl) noexcept
{
    dest.wqn = src.wqn;
    dest.bufAddr = src.buf_addr;
    dest.wqeSize = src.wqebb_size;
    dest.depth = src.depth;
    dest.headAddr = src.head_addr;
    dest.tailAddr = src.tail_addr;
    dest.dbMode = dbMode;
    if (dbMode == DBMode::SW_DB) {
        dest.dbAddr = src.swdb_addr;
    } else if (dbMode == DBMode::HW_DB) {
        dest.dbAddr = src.db_reg;
    }
    dest.sl = sl;
}

void FixedRanksQpManager::CopyAiCQInfo(struct AiQpRMACQ &dest, const ai_data_plane_cq &source, DBMode dbMode) noexcept
{
    dest.cqn = source.cqn;
    dest.bufAddr = source.buf_addr;
    dest.cqeSize = source.cqe_size;
    dest.depth = source.depth;
    dest.headAddr = source.head_addr;
    dest.tailAddr = source.tail_addr;
    dest.dbMode = dbMode;
    if (dbMode == DBMode::SW_DB) {
        dest.dbAddr = source.swdb_addr;
    } else if (dbMode == DBMode::HW_DB) {
        dest.dbAddr = source.db_reg;
    }
}

void FixedRanksQpManager::CloseServices() noexcept
{
    /* wake gate waiters first and stop the build worker promptly (its polls watch started_) */
    started_.store(false);
    stateCv_.notify_all();
    if (buildThread_ != nullptr) {
        if (buildThread_->joinable()) {
            buildThread_->join();
        }
        buildThread_ = nullptr;
    }

    CloseServerConnections();
    CloseClientConnections();
}

void FixedRanksQpManager::CloseClientConnections() noexcept
{
    CloseConnections(clientConnections_);
}

void FixedRanksQpManager::CloseServerConnections() noexcept
{
    /* close per-channel resources first, while the shared listen handle is still alive
     * (BatchClose dereferences socketOps through it); DestroyServerSocket, the sole owner,
     * then deinits the listen handle exactly once */
    CloseConnections(serverConnections_);
    DestroyServerSocket();
}

void FixedRanksQpManager::CloseConnections(std::unordered_map<uint32_t, AiCoreConnChannel> &connections) noexcept
{
    std::vector<HccpSocketCloseInfo> socketCloseInfos;
    for (auto it = connections.begin(); it != connections.end(); ++it) {
        for (uint32_t lane = 0; lane < qpsPerPeer_; lane++) {
            if (it->second.qpHandle[lane] != nullptr) {
                auto ret = DlHccpApi::RaQpDestroy(it->second.qpHandle[lane]);
                if (ret != 0) {
                    BM_LOG_WARN("destroy AI QP lane " << lane << " to server: " << it->first
                                       << " failed: " << ret);
                }
                it->second.qpHandle[lane] = nullptr;
            }

            if (it->second.socketFd[lane] != nullptr) {
                HccpSocketCloseInfo info;
                info.handle = it->second.socketHandle;
                info.fd = it->second.socketFd[lane];
                info.linger = 0;
                socketCloseInfos.push_back(info);
                it->second.socketFd[lane] = nullptr;
            }
        }
    }

    if (!socketCloseInfos.empty()) {
        auto ret = DlHccpApi::RaSocketBatchClose(socketCloseInfos.data(), socketCloseInfos.size());
        if (ret != 0) {
            BM_LOG_INFO("close sockets return: " << ret);
        }
    }

    for (auto it = connections.begin(); it != connections.end(); ++it) {
        /* server channels borrow the shared listen handle (serverSocketHandle_, registered
         * per entry in GenerateWhiteList); its lifetime is owned by DestroyServerSocket.
         * Deinit-ing it here double-frees: RaSocketDeinit NULLs ops before free, so a second
         * call dereferences the NULL ops and crashes inside libra. */
        if (it->second.socketHandle != nullptr && it->second.socketHandle != serverSocketHandle_) {
            auto ret = DlHccpApi::RaSocketDeinit(it->second.socketHandle);
            if (ret != 0) {
                BM_LOG_INFO("deinit socket to server: " << it->first << " return: " << ret);
            }
        }
    }

    connections.clear();
}
} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock
