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
#include <string>
#include <vector>
#include "hybm_logger.h"
#include "dl_acl_api.h"
#include "dl_hccp_api.h"
#include "mf_env_util.h"
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
         * entries (memKeys grow per slice) instead of rejecting, then hand the transport work to
         * the establishment loops. This callback runs inside the group JoinHandle and MUST NOT
         * wait on the network: the join round's barrier vote used to block here on connect/QP
         * readiness, which coupled with the hccp whitelist race into the FSDP deadlock (a late
         * joiner's round never completed, the version sequence froze, everyone queued behind).
         * Failures are not possible by design -- ranks stay pending until their channel is up. */
        for (auto it = ranks.begin(); it != ranks.end(); ++it) {
            currentRanksInfo_.insert_or_assign(it->first, it->second);
        }
        {
            std::lock_guard<std::mutex> lock(loopMutex_);
            for (auto it = currentRanksInfo_.begin(); it != currentRanksInfo_.end(); ++it) {
                if (it->first == rankId_) {
                    continue;
                }
                if (it->first < rankId_) {
                    if (!RankMeshReady(it->first)) {
                        pendingClientRanks_.insert(it->first);
                    }
                } else if (!RankMeshReady(it->first)) {
                    pendingServerRanks_.insert(it->first);
                }
            }
        }
        loopCv_.notify_all();
        DlAclApi::AclrtSetDevice(deviceId_);
        if (FillQpInfo() != BM_OK) {
            BM_LOG_WARN("refresh qp info after rank update failed; deferred to connect worker");
        }
        return BM_OK;
    }

    currentRanksInfo_ = ranks;
    return BM_OK;
}

bool FixedRanksQpManager::RankMeshReady(uint32_t rankId) const noexcept
{
    /* fully established = lane 0 socket + QP both present (lanes >0 are best-effort addons
     * riding the same endpoint; CheckQpReady enforces them per lane for the data-plane gate) */
    std::shared_lock<std::shared_mutex> lock(channelsMutex_);
    auto cit = clientConnections_.find(rankId);
    if (cit != clientConnections_.end()) {
        return (cit->second.socketFd[0] != nullptr) && (cit->second.qpHandle[0] != nullptr);
    }
    auto sit = serverConnections_.find(rankId);
    if (sit != serverConnections_.end()) {
        return (sit->second.socketFd[0] != nullptr) && (sit->second.qpHandle[0] != nullptr);
    }
    return false;
}

void FixedRanksQpManager::ClientRunLoop() noexcept
{
    DlAclApi::AclrtSetDevice(deviceId_);
    while (running_.load()) {
        std::unique_lock<std::mutex> lock(loopMutex_);
        loopCv_.wait_for(lock, std::chrono::milliseconds(300));
        if (pendingClientRanks_.empty() && running_.load()) {
            /* idle park; any SetRemoteRankInfo notify wakes us immediately */
            loopCv_.wait_for(lock, std::chrono::seconds(60));
        }
        if (!running_.load() || pendingClientRanks_.empty()) {
            continue;
        }
        auto targets = pendingClientRanks_;
        lock.unlock();
        if (!ClientSideCatchUp(targets)) {
            /* failed round: back off briefly so a persistent failure (peer down / driver
             * error) does not turn into a 300ms log storm; the pending set still retries */
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

void FixedRanksQpManager::ServerRunLoop() noexcept
{
    DlAclApi::AclrtSetDevice(deviceId_);
    while (running_.load()) {
        std::unique_lock<std::mutex> lock(loopMutex_);
        loopCv_.wait_for(lock, std::chrono::milliseconds(300));
        if (pendingServerRanks_.empty() && running_.load()) {
            loopCv_.wait_for(lock, std::chrono::seconds(60));
        }
        if (!running_.load() || pendingServerRanks_.empty()) {
            continue;
        }
        auto targets = pendingServerRanks_;
        lock.unlock();
        if (!ServerSideCatchUp(targets)) {
            /* failed round: back off (same rationale as the client loop) */
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

uint32_t FixedRanksQpManager::RoundBudgetSec() noexcept
{
    /* one establishment round's wait budget; MF_CONN_ROUND_TIMEOUT (seconds) overrides.
     * Short by default: rounds RETRY, so a refused/racy connect is re-issued within
     * seconds instead of being parked behind a minutes-long wait */
    const uint32_t budget = MfEnvUtil::GetOptionalUintOrDefault("MF_CONN_ROUND_TIMEOUT", 5U);
    return budget == 0U ? 1U : budget;
}

bool FixedRanksQpManager::ClientSideCatchUp(const std::set<uint32_t> &targets) noexcept
{
    /* one establishment round for the pending smaller-ranked peers:
     *  1. ranks without a channel yet: create the local socket handle + issue the batch
     *     connect (incl. tagged lanes);
     *  2. wait for the sockets + build the QPs (whole-map walk, idempotent);
     *  3. ranks whose socket never arrived (whitelist race lost this round -- the peer's
     *     server adds our IP microseconds later) are dropped from the map again so the
     *     NEXT round re-creates the handle and re-issues the connect: that retry is what
     *     heals the race. Everything stays pending until fully established. */
    std::vector<std::pair<uint32_t, void *>> freshChannels;
    {
        std::shared_lock<std::shared_mutex> lock(channelsMutex_);
        for (auto rank : targets) {
            if (clientConnections_.find(rank) == clientConnections_.end()) {
                auto infoIt = currentRanksInfo_.find(rank);
                if (infoIt != currentRanksInfo_.end()) {
                    freshChannels.emplace_back(rank, nullptr);
                }
            }
        }
    }
    if (!freshChannels.empty()) {
        std::vector<HccpSocketConnectInfo> connectInfos;
        std::vector<void *> createdHandles;
        for (auto &entry : freshChannels) {
            auto infoIt = currentRanksInfo_.find(entry.first);
            auto socketHandle = CreateLocalSocket();
            if (socketHandle == nullptr) {
                BM_LOG_WARN("client round: create local socket for rank " << entry.first
                              << " failed; retrying next round");
                entry.second = nullptr;
                continue;
            }
            entry.second = socketHandle;
            createdHandles.emplace_back(socketHandle);
            HccpSocketConnectInfo connectInfo{};
            connectInfo.handle = socketHandle;
            connectInfo.remoteIp.addr = infoIt->second.network.sin_addr;
            connectInfo.port = infoIt->second.network.sin_port;
            bzero(connectInfo.tag, sizeof(connectInfo.tag));
            connectInfos.emplace_back(connectInfo);
            if (qpsPerPeer_ > 1U) {
                /* extra tagged lanes ride the SAME (ip, port) endpoint and the SAME local
                 * handle; the tag derives from our own rank (we are the client) */
                for (uint32_t lane = 1; lane < qpsPerPeer_; lane++) {
                    HccpSocketConnectInfo extra = connectInfo;
                    MakeLaneTag(extra.tag, sizeof(extra.tag), rankId_, lane);
                    connectInfos.emplace_back(extra);
                }
            }
        }
        if (connectInfos.empty()) {
            return false;
        }
        auto ret = DlHccpApi::RaSocketBatchConnect(connectInfos.data(), connectInfos.size());
        if (ret != 0) {
            BM_LOG_WARN("client round: batch connect failed: " << ret << "; retrying next round");
            for (auto handle : createdHandles) {
                (void)DlHccpApi::RaSocketDeinit(handle);
            }
            return false;
        }
        {
            std::unique_lock<std::shared_mutex> lock(channelsMutex_);
            for (auto &entry : freshChannels) {
                if (entry.second == nullptr) {
                    continue;
                }
                auto infoIt = currentRanksInfo_.find(entry.first);
                clientConnections_.emplace(entry.first,
                                           AiCoreConnChannel{infoIt->second.network.sin_addr, entry.second});
            }
        }
    }

    DlAclApi::AclrtSetDevice(deviceId_);
    const uint32_t roundBudget = RoundBudgetSec();
    if (WaitConnectionsReady(clientConnections_, roundBudget) != BM_OK) {
        BM_LOG_WARN("client round: connections not ready in time; recycling half-built channels"
                    " so the next round re-issues fresh connects");
        RecycleChannels(targets, false);
        return false;
    }
    if (CreateQpWaitingReady(clientConnections_, roundBudget) != BM_OK) {
        BM_LOG_WARN("client round: QP create/wait failed; recycling half-built channels for a"
                    " fresh rebuild (a broken QP handle must not survive the round)");
        RecycleChannels(targets, false);
        return false;
    }
    if (FillQpInfo() != BM_OK) {
        BM_LOG_WARN("client round: refresh qp info failed; deferred to next round");
    }
    size_t stillPending = 0;
    {
        std::lock_guard<std::mutex> lock(loopMutex_);
        for (auto rank : targets) {
            if (RankMeshReady(rank)) {
                pendingClientRanks_.erase(rank);
            }
        }
        stillPending = pendingClientRanks_.size();
    }
    BM_LOG_INFO("client round: connections established, " << stillPending << " peer(s) still pending");
    return true;
}

bool FixedRanksQpManager::ServerSideCatchUp(const std::set<uint32_t> &targets) noexcept
{
    /* one establishment round for the pending larger-ranked peers: whitelist their IPs
     * (idempotent -- only newly emplaced channels generate whitelist entries), then wait
     * for their connect + build the server QP halves. A peer whose connect was refused by
     * the not-yet-updated whitelist retries on ITS side; our whitelist entry persists in
     * hccp, so the next attempt lands. Ranks stay pending until fully established. */
    std::vector<HccpSocketWhiteListInfo> whitelist;
    std::vector<uint32_t> emplacedRanks;
    {
        std::unique_lock<std::shared_mutex> lock(channelsMutex_);
        for (auto rank : targets) {
            if (serverConnections_.find(rank) != serverConnections_.end() || serverSocketHandle_ == nullptr) {
                continue;
            }
            auto infoIt = currentRanksInfo_.find(rank);
            if (infoIt == currentRanksInfo_.end()) {
                continue;
            }
            HccpSocketWhiteListInfo info{};
            info.remoteIp.addr = infoIt->second.network.sin_addr;
            info.connLimit = rankCount_;
            bzero(info.tag, sizeof(info.tag));
            whitelist.emplace_back(info);
            serverConnections_.emplace(rank, AiCoreConnChannel{info.remoteIp.addr, serverSocketHandle_});
            emplacedRanks.emplace_back(rank);
            if (qpsPerPeer_ > 1U) {
                for (uint32_t lane = 1; lane < qpsPerPeer_; lane++) {
                    HccpSocketWhiteListInfo extra{};
                    extra.remoteIp.addr = infoIt->second.network.sin_addr;
                    extra.connLimit = rankCount_;
                    MakeLaneTag(extra.tag, sizeof(extra.tag), rank, lane);
                    whitelist.emplace_back(extra);
                }
            }
        }
    }
    if (!whitelist.empty()) {
        auto ret = DlHccpApi::RaSocketWhiteListAdd(serverSocketHandle_, whitelist.data(), whitelist.size());
        if (ret != 0) {
            /* roll the freshly emplaced channels back: a channel whose whitelist entry never
             * landed would be skipped by every later round (map hit) and never accept */
            BM_LOG_WARN("server round: whitelist add failed: " << ret << "; rolling back this round's"
                        " channels so the next round retries them together with the whitelist");
            std::unique_lock<std::shared_mutex> lock(channelsMutex_);
            for (auto rank : emplacedRanks) {
                serverConnections_.erase(rank);
            }
            return false;
        }
    }

    DlAclApi::AclrtSetDevice(deviceId_);
    const uint32_t roundBudget = RoundBudgetSec();
    if (WaitConnectionsReady(serverConnections_, roundBudget) != BM_OK) {
        BM_LOG_WARN("server round: accepts not ready in time; ranks stay pending for retry");
        return false;
    }
    if (CreateQpWaitingReady(serverConnections_, roundBudget) != BM_OK) {
        BM_LOG_WARN("server round: QP create/wait failed; recycling half-built channels for rebuild");
        RecycleChannels(targets, true);
        return false;
    }
    if (FillQpInfo() != BM_OK) {
        BM_LOG_WARN("server round: refresh qp info failed; deferred to next round");
    }
    size_t stillPending = 0;
    {
        std::lock_guard<std::mutex> lock(loopMutex_);
        for (auto rank : targets) {
            if (RankMeshReady(rank)) {
                pendingServerRanks_.erase(rank);
            }
        }
        stillPending = pendingServerRanks_.size();
    }
    BM_LOG_INFO("server round: connections established, " << stillPending << " peer(s) still pending");
    return true;
}

void FixedRanksQpManager::RecycleChannels(const std::set<uint32_t> &targets, bool serverSide) noexcept
{
    /* full teardown of the targets' half-built/broken channels: destroy lane QPs, close their
     * fds, deinit the client-owned connect handle, erase the entries -- the next round then
     * rebuilds them from scratch (fresh handle -> fresh connect -> fresh QP). Anything less
     * leaves a poisoned handle that every later round trips over: observed on the FSDP run as
     * "connect AI QP failed: 228102" followed by an endless "get AI QP status failed: 328107"
     * retry loop, because re-walks skip entries whose qpHandle is non-null. Fully
     * established channels (fd + QP + ready status) are kept untouched. */
    auto &connections = serverSide ? serverConnections_ : clientConnections_;
    std::vector<HccpSocketCloseInfo> closeInfos;
    uint32_t recycled = 0;
    {
        std::unique_lock<std::shared_mutex> lock(channelsMutex_);
        for (auto rank : targets) {
            auto it = connections.find(rank);
            if (it == connections.end()) {
                continue;
            }
            if (it->second.socketFd[0] != nullptr && it->second.qpHandle[0] != nullptr &&
                it->second.qpStatus[0] == 1) {
                continue; /* working channel: keep it */
            }
            for (uint32_t lane = 0; lane < qpsPerPeer_; lane++) {
                if (it->second.qpHandle[lane] != nullptr) {
                    auto ret = DlHccpApi::RaQpDestroy(it->second.qpHandle[lane]);
                    if (ret != 0) {
                        BM_LOG_WARN("recycle: destroy QP lane " << lane << " to rank " << rank
                                    << " failed: " << ret);
                    }
                    it->second.qpHandle[lane] = nullptr;
                    it->second.qpStatus[lane] = -1;
                }
                if (it->second.socketFd[lane] != nullptr) {
                    HccpSocketCloseInfo info;
                    info.handle = it->second.socketHandle;
                    info.fd = it->second.socketFd[lane];
                    info.linger = 0;
                    closeInfos.push_back(info);
                    it->second.socketFd[lane] = nullptr;
                }
            }
            /* same ownership rule as CloseConnections: server channels borrow the shared
             * listen handle owned by DestroyServerSocket and must not be deinit-ed here */
            if (it->second.socketHandle != nullptr && it->second.socketHandle != serverSocketHandle_) {
                auto ret = DlHccpApi::RaSocketDeinit(it->second.socketHandle);
                if (ret != 0) {
                    BM_LOG_WARN("recycle: deinit client socket to rank " << rank << " return: " << ret);
                }
            }
            connections.erase(it);
            recycled++;
        }
    }
    if (!closeInfos.empty()) {
        auto ret = DlHccpApi::RaSocketBatchClose(closeInfos.data(), closeInfos.size());
        if (ret != 0) {
            BM_LOG_INFO("recycle: batch close sockets return: " << ret);
        }
    }
    if (recycled > 0) {
        BM_LOG_INFO((serverSide ? "server" : "client") << " round: recycled " << recycled
                    << " half-built channel(s) for a fresh rebuild");
    }
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
     * as the connect worker threads in StartServerSide/StartClientSide. */
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

    /* synchronous part: local resources only (listen socket + initial whitelist + local
     * socket-handle pre-creation). Every network interaction -- batch connect, accept
     * waits, QP handshakes -- belongs to the run-loops below, never to this stack (it may
     * be a dynamic-group JoinHandle, where blocking was the FSDP deadlock's root). */
    auto ret = StartServerSide();
    if (ret != BM_OK) {
        BM_LOG_ERROR("start server side failed: " << ret);
        return ret;
    }

    ret = StartClientSide();
    if (ret != BM_OK) {
        BM_LOG_ERROR("start client side failed: " << ret);
        return ret;
    }

    running_.store(true);
    if (rankId_ + 1U != rankCount_) {
        serverConnectThread_ = std::make_shared<std::thread>([this]() { ServerRunLoop(); });
    }
    if (rankId_ != 0U) {
        clientConnectThread_ = std::make_shared<std::thread>([this]() { ClientRunLoop(); });
    }
    started_.store(true);
    loopCv_.notify_all();
    return BM_OK;
}

void FixedRanksQpManager::Shutdown() noexcept
{
    CloseServices();
    ReleaseQpInfoSpace();
}

int FixedRanksQpManager::WaitingConnectionReady() noexcept
{
    /* non-blocking by design (Joinable-model contract): wake the establishment loops and
     * return. Readiness is gated downstream by RdmaTransportManager::WaitQpReady polling
     * CheckQpReady (bounded 30s + 100ms/rank), which never runs on a group-event thread
     * without the pending-set safety net below it. The previous implementation joined the
     * one-shot worker threads here and blocked the dynamic-group JoinHandle for the whole
     * connect+QP window -- one edge of the FSDP join-round deadlock. */
    if (!started_.load()) {
        BM_LOG_WARN("waiting connection ready called before startup; establishment loops not running");
        return BM_OK;
    }
    loopCv_.notify_all();
    size_t clientPending = 0;
    size_t serverPending = 0;
    {
        std::lock_guard<std::mutex> lock(loopMutex_);
        clientPending = pendingClientRanks_.size();
        serverPending = pendingServerRanks_.size();
    }
    BM_LOG_INFO("connection establishment delegated to background loops"
                " (client pending: " << clientPending << ", server pending: " << serverPending << ")");
    return BM_OK;
}

bool FixedRanksQpManager::CheckQpReady(const std::vector<uint32_t> &rankIds) const noexcept
{
    /* data-plane gate polled by RdmaTransportManager::WaitQpReady: a rank counts as ready
     * only when EVERY lane of its channel reached QP-ready state (status 1); lanes that
     * were never attempted stay -1 and keep the gate closed */
    std::shared_lock<std::shared_mutex> lock(channelsMutex_);
    for (auto rankId : rankIds) {
        if (rankId == rankId_) {
            continue;
        }
        if (rankId >= rankCount_) {
            BM_LOG_ERROR("check qp ready got invalid rank id: " << rankId << ", rank count: " << rankCount_);
            return false;
        }
        const AiCoreConnChannel *channel = nullptr;
        auto cit = clientConnections_.find(rankId);
        if (cit != clientConnections_.end()) {
            channel = &cit->second;
        } else {
            auto sit = serverConnections_.find(rankId);
            if (sit != serverConnections_.end()) {
                channel = &sit->second;
            }
        }
        if (channel == nullptr) {
            return false;
        }
        for (uint32_t lane = 0; lane < qpsPerPeer_; lane++) {
            if (channel->qpStatus[lane] != 1) {
                return false;
            }
        }
    }
    return true;
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
        serverConnectResult = 0;
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

    /* larger ranks known at startup already carry a whitelist entry + channel (GenerateWhiteList);
     * their accepts and server-side QP halves are served by the persistent server run-loop,
     * NOT by a one-shot thread that WaitingConnectionReady used to join -- that join was the
     * group-handler blocking edge of the FSDP join-round deadlock */
    {
        std::lock_guard<std::mutex> lock(loopMutex_);
        for (auto it = currentRanksInfo_.begin(); it != currentRanksInfo_.end(); ++it) {
            if (it->first > rankId_) {
                pendingServerRanks_.insert(it->first);
            }
        }
    }
    serverConnectResult = BM_OK;
    return BM_OK;
}

int FixedRanksQpManager::StartClientSide() noexcept
{
    if (rankId_ == 0U) {
        BM_LOG_INFO("rankId: " << rankId_ << " need not connect to others.");
        clientConnectResult = BM_OK;
        return BM_OK;
    }

    /* pre-create the LOCAL socket handles only -- fail-fast on driver resource errors while
     * the caller can still propagate them. The batch connect itself is deferred to the
     * client run-loop: issuing it on this stack (a dynamic-group JoinHandle) is exactly the
     * whitelist-race pattern that deadlocked multi-client joins on the FSDP cluster (the
     * peer's whitelist add lands microseconds after our connect fires). */
    std::vector<std::pair<uint32_t, void *>> created;
    for (auto it = currentRanksInfo_.begin(); it != currentRanksInfo_.end(); ++it) {
        if (it->first >= rankId_) {
            continue; // client connect to small ranks.
        }
        auto socketHandle = CreateLocalSocket();
        if (socketHandle == nullptr) {
            BM_LOG_ERROR("create local socket handle failed.");
            for (auto &entry : created) {
                (void)DlHccpApi::RaSocketDeinit(entry.second);
            }
            return BM_DL_FUNCTION_FAILED;
        }
        created.emplace_back(it->first, socketHandle);
    }
    if (created.empty()) {
        /* no smaller-ranked peer in the member table yet (dynamic groups: we are the lowest
         * rank so far) -- the run-loop connects them as their imports arrive */
        BM_LOG_INFO("no smaller-ranked peers in the member table yet (rank " << rankId_
                    << " of " << rankCount_ << "), client connections deferred to background loop");
        clientConnectResult = BM_OK;
        return BM_OK;
    }
    {
        std::unique_lock<std::shared_mutex> lock(channelsMutex_);
        for (auto &entry : created) {
            auto infoIt = currentRanksInfo_.find(entry.first);
            clientConnections_.emplace(entry.first,
                                       AiCoreConnChannel{infoIt->second.network.sin_addr, entry.second});
        }
    }
    {
        std::lock_guard<std::mutex> lock(loopMutex_);
        for (auto &entry : created) {
            pendingClientRanks_.insert(entry.first);
        }
    }
    clientConnectResult = BM_OK;
    return BM_OK;
}

int FixedRanksQpManager::GenerateWhiteList() noexcept
{
    std::vector<HccpSocketWhiteListInfo> whitelist;
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

int FixedRanksQpManager::WaitConnectionsReady(std::unordered_map<uint32_t, AiCoreConnChannel> &connections,
                                               uint32_t roundBudgetSec) noexcept
{
    /* count entries that are ALREADY settled on entry: establishment rounds
     * re-walk the whole map, and previously settled connections must satisfy the loop condition
     * from the start. Without this base count the loop never terminates once a mixed map settles,
     * and the next RaGetSockets runs on an EMPTY list, which hccp rejects with 128203 -- the fd
     * WAS ready but the caller treated it as failure and skipped QP creation, deadlocking the
     * whole join barrier (observed on the dual-client run: peer connect to (0) ready followed
     * by role(1) get sockets failed: 128203). */
    uint32_t totalSuccessCount = 0;
    for (auto it = connections.begin(); it != connections.end(); ++it) {
        if (it->second.socketFd[0] != nullptr) {
            totalSuccessCount++;
        }
    }
    /* per-round budget: the establishment loops call this with a SHORT budget (default 5s,
     * MF_CONN_ROUND_TIMEOUT) so a lost whitelist race is retried within seconds -- the old
     * 2-minute default made sense when this ran on the group-handler stack, which it must
     * never do again (see the class comment). The round budget, not patience, heals races. */
    auto start = std::chrono::steady_clock::now();
    auto timeout = start + std::chrono::seconds(roundBudgetSec);
    while (totalSuccessCount < connections.size()) {
        if (std::chrono::steady_clock::now() >= timeout) {
            std::string pending;
            for (auto it = connections.begin(); it != connections.end(); ++it) {
                if (it->second.socketFd[0] == nullptr) {
                    pending += std::to_string(it->first) + " ";
                }
            }
            BM_LOG_ERROR("waiting connection ready timeout after " << roundBudgetSec
                        << "s round budget, " << (connections.size() - totalSuccessCount)
                        << " peer(s) not ready: " << pending);
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
        if (socketInfos.empty()) {
            /* nothing left to query this round (settled by the base count above) */
            continue;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        auto role = (&connections == &clientConnections_) ? 1 : 0;
        auto ret = DlHccpApi::RaGetSockets(role, socketInfos.data(), socketInfos.size(), successCount);
        if (ret != 0) {
            if (ret == 128203) {
                /* hccp reports "no completed connection to hand out yet" as this code on the
                 * server side; in the round model that is ordinary "no progress this poll",
                 * not an error -- keep polling within the budget (a 300ms WARN storm was
                 * observed otherwise) */
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

int FixedRanksQpManager::CreateQpWaitingReady(std::unordered_map<uint32_t, AiCoreConnChannel> &connections,
                                               uint32_t roundBudgetSec) noexcept
{
    const bool clientSide = (&connections == &clientConnections_);
    /* lane 0: legacy primary connections, one QP per rank; skip entries whose QP already
     * exists -- establishment rounds re-walk the whole map */
    for (auto it = connections.begin(); it != connections.end(); ++it) {
        if (it->second.qpHandle[0] != nullptr) {
            continue;
        }
        auto ret = CreateOneQp(it->second, 0);
        if (ret != 0) {
            BM_LOG_ERROR("create QP  to " << it->first << " failed: " << ret);
            /* a failed create must not leave a stale handle the next round's re-walk
             * would skip over */
            it->second.qpHandle[0] = nullptr;
            return BM_DL_FUNCTION_FAILED;
        }

        ret = DlHccpApi::RaQpConnectAsync(it->second.qpHandle[0], it->second.socketFd[0]);
        if (ret != 0) {
            BM_LOG_ERROR("connect AI QP to " << it->first << " failed: " << ret);
            /* destroy on the spot: keeping a connect-failed QP poisons every later poll
             * (observed: endless get-QP-status 328107 after connect 228102) */
            (void)DlHccpApi::RaQpDestroy(it->second.qpHandle[0]);
            it->second.qpHandle[0] = nullptr;
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
                if (channel.qpHandle[lane] != nullptr) {
                    continue; /* already created by an earlier round (incremental path) */
                }
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
                    channel.qpHandle[lane] = nullptr; /* same stale-handle rule as lane 0 */
                    return BM_DL_FUNCTION_FAILED;
                }
                ret = DlHccpApi::RaQpConnectAsync(channel.qpHandle[lane], laneFd);
                if (ret != 0) {
                    BM_LOG_ERROR("connect lane " << lane << " QP to " << it->first << " failed: " << ret);
                    (void)DlHccpApi::RaQpDestroy(channel.qpHandle[lane]);
                    channel.qpHandle[lane] = nullptr;
                    return BM_DL_FUNCTION_FAILED;
                }
                BM_LOG_INFO("multi-QP " << (clientSide ? "client" : "server") << " rank " << it->first
                                            << " lane " << lane << " fd " << laneFd << " tag '" << tag << "'");
            }
        }
    }

    /* per-round budget (same semantics as WaitConnectionsReady's): short by default so the
     * loop retries quickly; the old 60s/180s hard/long waits belonged to the handler-stack
     * era and only delayed self-healing of the whitelist race */
    auto start = std::chrono::steady_clock::now();
    auto timeout = start + std::chrono::seconds(roundBudgetSec);
    while (std::chrono::steady_clock::now() < timeout) {
        int connectingCount = 0;
        for (auto it = connections.begin(); it != connections.end(); ++it) {
            for (uint32_t lane = 0; lane < qpsPerPeer_; lane++) {
                int status = 0;
                auto ret = DlHccpApi::RaGetQpStatus(it->second.qpHandle[lane], status);
                if (ret != 0) {
                    BM_LOG_ERROR("get AI QP status to " << it->first << " lane " << lane << " failed: " << ret
                                << "; dropping the broken QP so the round can recycle the channel");
                    if (it->second.qpHandle[lane] != nullptr) {
                        (void)DlHccpApi::RaQpDestroy(it->second.qpHandle[lane]);
                        it->second.qpHandle[lane] = nullptr;
                        it->second.qpStatus[lane] = -1;
                    }
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
    /* fail loudly with the stuck peer list: one not-ready lane stalls the whole mesh, and
     * "which peer" was the missing bit of evidence in the FSDP -7 post-mortem */
    std::string stuck;
    for (auto it = connections.begin(); it != connections.end(); ++it) {
        for (uint32_t lane = 0; lane < qpsPerPeer_; lane++) {
            if (it->second.qpStatus[lane] != 1) {
                stuck += std::to_string(it->first) + ":L" + std::to_string(lane) + "=" +
                         std::to_string(it->second.qpStatus[lane]) + " ";
            }
        }
    }
    BM_LOG_ERROR("QP ready wait timed out after " << roundBudgetSec
                << "s round budget, stuck peers [rank:lane=status]: " << stuck);
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

int FixedRanksQpManager::FillQpInfo() noexcept
{
    if (qpInfoSize_ == 0U || qpInfo_ == nullptr) {
        /* reachable when a rank update arrives while the table is not reserved yet (e.g. tests
         * or a startup race): the zero-size buffer below would hand out a null data pointer.
         * The SetRemoteRankInfo refresh path treats this as a benign skip (warn + deferred) */
        BM_LOG_WARN("fill qp info skipped: qp table not reserved yet");
        return BM_ERROR;
    }
    /* channel maps are structure-mutated by the establishment loops; snapshot-read under the
     * shared lock. Callers must NOT hold channelsMutex_ (none does) */
    std::shared_lock<std::shared_mutex> channelsLock(channelsMutex_);
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
            BM_LOG_ERROR("missing for remote: " << it->first);
            return BM_ERROR;
        }

        for (uint32_t lane = 0; lane < qpsPerPeer_; lane++) {
            const auto &laneInfo = pos->second.aiQpInfo[lane].data_plane_info;
            CopyAiWQInfo(copyInfo->sq[it->first * qpsPerPeer_ + lane], laneInfo.sq, DBMode::HW_DB, COPY_INFO_SL);
            CopyAiWQInfo(copyInfo->rq[it->first * qpsPerPeer_ + lane], laneInfo.rq, DBMode::SW_DB, COPY_INFO_SL);
            CopyAiCQInfo(copyInfo->scq[it->first * qpsPerPeer_ + lane], laneInfo.scq, DBMode::HW_DB);
            CopyAiCQInfo(copyInfo->rcq[it->first * qpsPerPeer_ + lane], laneInfo.rcq, DBMode::SW_DB);
        }
    }

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
    /* stop the establishment loops first so no worker touches the channel maps while we
     * tear them down; the cv wake shortens the idle park to at most one 300ms tick */
    running_.store(false);
    loopCv_.notify_all();
    if (serverConnectThread_ != nullptr) {
        serverConnectThread_->join();
        serverConnectThread_ = nullptr;
    }

    if (clientConnectThread_ != nullptr) {
        clientConnectThread_->join();
        clientConnectThread_ = nullptr;
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
