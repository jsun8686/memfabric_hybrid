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
         * QP/MR table. Connections stay as-is; a rank not connected yet is filled by the connect
         * worker's own FillQpInfo once its QP becomes ready, so a refresh failure is not fatal. */
        {
            std::lock_guard<std::mutex> lock(mapsMutex_);
            for (auto it = ranks.begin(); it != ranks.end(); ++it) {
                currentRanksInfo_.insert_or_assign(it->first, it->second);
            }
        }
        /* Incremental whitelist for larger-ranked peers that appeared AFTER Startup:
         * GenerateWhiteList only ran once with the then-current member table (late clients in
         * example 14, staggered NEAR creates and the FAR contributor in FSDP all hit this).
         * Added synchronously here on the group-event thread: a local, microsecond-level hccp
         * call that completes before this join round's barrier, while the peer's connect is a
         * cross-node network round -- the race window is milliseconds and points in our favor;
         * the client worker's connect retry covers the rest. The channel entry is NOT added
         * here: the (persistent) server worker owns its map and picks the peer up in its own
         * member-table scan on the next retry round. */
        std::vector<HccpSocketWhiteListInfo> whitelist;
        {
            std::lock_guard<std::mutex> lock(mapsMutex_);
            for (auto it = ranks.begin(); it != ranks.end(); ++it) {
                if (it->first <= rankId_ || serverSocketHandle_ == nullptr ||
                    serverConnections_.find(it->first) != serverConnections_.end()) {
                    continue;
                }
                whitelist.emplace_back(HccpSocketWhiteListInfo{});
                whitelist.back().remoteIp.addr = it->second.network.sin_addr;
                whitelist.back().connLimit = rankCount_;
                bzero(whitelist.back().tag, sizeof(whitelist.back().tag));
                for (uint32_t lane = 1; lane < qpsPerPeer_; lane++) {
                    HccpSocketWhiteListInfo extra{};
                    extra.remoteIp.addr = it->second.network.sin_addr;
                    extra.connLimit = rankCount_;
                    MakeLaneTag(extra.tag, sizeof(extra.tag), it->first, lane);
                    whitelist.emplace_back(extra);
                }
            }
        }
        if (!whitelist.empty()) {
            auto ret = DlHccpApi::RaSocketWhiteListAdd(serverSocketHandle_, whitelist.data(), whitelist.size());
            if (ret != 0) {
                /* not fatal: the next import (every UpdateRankOptions round) re-runs this, and
                 * the peer's client worker retries its connect anyway */
                BM_LOG_WARN("incremental whitelist add failed: " << ret << "; next import retries");
            }
        }
        DlAclApi::AclrtSetDevice(deviceId_);
        if (FillQpInfo() != BM_OK) {
            BM_LOG_WARN("refresh qp info after rank update failed; deferred to connect worker");
        }
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

    /* set before the workers start: their retry loops check this flag on every pass */
    workersRunning_.store(true);
    auto ret = StartServerSide();
    if (ret != BM_OK) {
        BM_LOG_ERROR("start server side failed: " << ret);
        workersRunning_.store(false);
        return ret;
    }

    ret = StartClientSide();
    if (ret != BM_OK) {
        BM_LOG_ERROR("start client side failed: " << ret);
        return ret;
    }

    started_.store(true);
    return BM_OK;
}

void FixedRanksQpManager::Shutdown() noexcept
{
    CloseServices();
    ReleaseQpInfoSpace();
}

int FixedRanksQpManager::WaitingConnectionReady() noexcept
{
    /* non-blocking BY DESIGN: this runs on the group-event thread inside hybm_import, BEFORE
     * the join round's barrier. The workers' success precondition -- the peer's whitelist
     * covering us -- only becomes certain after that barrier, so joining the (retrying)
     * workers here froze the group engine's serial event queue for the whole connect window:
     * the FSDP join-round deadlock. The workers retry until established; data-plane readiness
     * is gated by WaitQpReady at the RemoteIO entry (CheckQpReady), never here. */
    BM_LOG_INFO("connection establishment delegated to retrying background workers (server result: "
                << serverConnectResult.load() << ", client result: " << clientConnectResult.load() << ")");
    return BM_OK;
}

bool FixedRanksQpManager::CheckQpReady(const std::vector<uint32_t> &rankIds) const noexcept
{
    /* real readiness gate for the data plane (WaitQpReady polls this): every rank that has
     * memory regions must have a channel with ALL lanes' QPs ready. The base-class default
     * returning true was only safe under the old join-worker synchronous model */
    std::lock_guard<std::mutex> lock(mapsMutex_);
    for (auto rankId : rankIds) {
        if (rankId == rankId_) {
            continue;
        }
        const auto *connections = (rankId < rankId_) ? &clientConnections_ : &serverConnections_;
        auto pos = connections->find(rankId);
        if (pos == connections->end()) {
            return false;
        }
        for (uint32_t lane = 0; lane < qpsPerPeer_; lane++) {
            if (pos->second.qpHandle[lane] == nullptr || pos->second.qpStatus[lane] != 1) {
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

    serverConnectThread_ = std::make_shared<std::thread>([this]() { ServerWorkerLoop(); });

    return BM_OK;
}

void FixedRanksQpManager::ServerWorkerLoop() noexcept
{
    DlAclApi::AclrtSetDevice(deviceId_);
    while (workersRunning_.load()) {
        /* pick up channels for peers whitelisted after Startup (SetRemoteRankInfo adds the
         * whitelist entry synchronously; the channel lands here when it could not be added
         * while a previous worker instance was alive) */
        {
            std::lock_guard<std::mutex> lock(mapsMutex_);
            for (auto it = currentRanksInfo_.begin(); it != currentRanksInfo_.end(); ++it) {
                if (it->first > rankId_ && serverSocketHandle_ != nullptr &&
                    serverConnections_.find(it->first) == serverConnections_.end()) {
                    serverConnections_.emplace(it->first,
                                               AiCoreConnChannel{it->second.network.sin_addr, serverSocketHandle_});
                }
            }
        }

        auto ret = WaitConnectionsReady(serverConnections_);
        if (ret == BM_OK) {
            ret = CreateQpWaitingReady(serverConnections_);
            if (ret == BM_OK) {
                serverConnectResult = BM_OK;
                if (FillQpInfo() != BM_OK) {
                    BM_LOG_WARN("server worker refresh qp info failed; next round retries");
                }
                /* settled: poll slowly for late joiners (the scan above picks them up) */
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
        }
        serverConnectResult = ret;
        /* failed attempt: drop ONLY the not-yet-accepted channels (no fd/QP resources behind
         * them; their peers' client workers retry the connect and a revived server worker or
         * the next import re-creates them); established ones survive */
        {
            std::lock_guard<std::mutex> lock(mapsMutex_);
            for (auto it = serverConnections_.begin(); it != serverConnections_.end();) {
                if (it->second.socketFd[0] == nullptr) {
                    it = serverConnections_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        BM_LOG_WARN("server accept attempt failed: " << ret << "; retrying in 1s");
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

int FixedRanksQpManager::StartClientSide() noexcept
{
    if (rankId_ == 0U) {
        BM_LOG_INFO("rankId: " << rankId_ << " need not connect to others.");
        clientConnectResult = BM_OK;
        return BM_OK;
    }

    /* No synchronous BatchConnect here: it fired on the Startup stack (group-event thread,
     * before the join barrier) while the target peers were still adding our whitelist entry
     * on theirs -- the whitelist race. The client worker owns the connects and retries them
     * until they land (whitelists are persistent in hccp). */
    clientConnectThread_ = std::make_shared<std::thread>([this]() { ClientWorkerLoop(); });
    return BM_OK;
}

void FixedRanksQpManager::ClientWorkerLoop() noexcept
{
    DlAclApi::AclrtSetDevice(deviceId_);
    while (workersRunning_.load()) {
        /* connect smaller-ranked peers that have no channel yet (dynamic groups add peers
         * long after Startup); retrying from a clean handle each failed attempt */
        std::vector<std::pair<uint32_t, sockaddr_in>> targets;
        {
            std::lock_guard<std::mutex> lock(mapsMutex_);
            for (auto it = currentRanksInfo_.begin(); it != currentRanksInfo_.end(); ++it) {
                if (it->first < rankId_ && clientConnections_.find(it->first) == clientConnections_.end()) {
                    targets.emplace_back(it->first, it->second.network);
                }
            }
        }
        if (!targets.empty()) {
            std::vector<HccpSocketConnectInfo> connectInfos;
            std::vector<uint32_t> added;
            for (auto &target : targets) {
                auto socketHandle = CreateLocalSocket();
                if (socketHandle == nullptr) {
                    BM_LOG_WARN("create local socket for rank " << target.first << " failed; retry later");
                    continue;
                }
                HccpSocketConnectInfo connectInfo{};
                connectInfo.handle = socketHandle;
                connectInfo.remoteIp.addr = target.second.sin_addr;
                connectInfo.port = target.second.sin_port;
                bzero(connectInfo.tag, sizeof(connectInfo.tag));
                connectInfos.emplace_back(connectInfo);
                if (qpsPerPeer_ > 1U) {
                    /* extra tagged lanes ride the same (ip, port) endpoint and handle; the
                     * tag derives from our own rank (we are the client) */
                    for (uint32_t lane = 1; lane < qpsPerPeer_; lane++) {
                        HccpSocketConnectInfo extra = connectInfo;
                        MakeLaneTag(extra.tag, sizeof(extra.tag), rankId_, lane);
                        connectInfos.emplace_back(extra);
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(mapsMutex_);
                    clientConnections_.emplace(target.first,
                                               AiCoreConnChannel{target.second.sin_addr, socketHandle});
                }
                added.emplace_back(target.first);
            }
            if (!connectInfos.empty()) {
                auto ret = DlHccpApi::RaSocketBatchConnect(connectInfos.data(), connectInfos.size());
                if (ret != 0) {
                    /* drop only THIS attempt's half-added entries (client owns its handles) */
                    std::lock_guard<std::mutex> lock(mapsMutex_);
                    for (auto rank : added) {
                        auto pos = clientConnections_.find(rank);
                        if (pos != clientConnections_.end()) {
                            (void)DlHccpApi::RaSocketDeinit(pos->second.socketHandle);
                            clientConnections_.erase(pos);
                        }
                    }
                    BM_LOG_WARN("batch connect failed: " << ret << "; retrying in 1s");
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    continue;
                }
            }
        }

        bool hasAnyClient = false;
        {
            std::lock_guard<std::mutex> lock(mapsMutex_);
            hasAnyClient = !clientConnections_.empty();
        }
        if (!hasAnyClient) {
            /* no smaller-ranked peer in the member table yet (FAR joins late in FSDP,
             * example 14's staggered clients): poll until one appears */
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        auto ret = WaitConnectionsReady(clientConnections_);
        if (ret == BM_OK) {
            ret = CreateQpWaitingReady(clientConnections_);
            if (ret == BM_OK) {
                clientConnectResult = BM_OK;
                if (FillQpInfo() != BM_OK) {
                    BM_LOG_WARN("client worker refresh qp info failed; next round retries");
                }
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
        }
        clientConnectResult = ret;
        /* failed attempt: tear down ONLY the not-yet-established half (fd pending or QP not
         * ready), keeping working channels; the next attempt re-connects them fresh */
        {
            std::vector<HccpSocketCloseInfo> closeInfos;
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
            if (!closeInfos.empty()) {
                auto cret = DlHccpApi::RaSocketBatchClose(closeInfos.data(), closeInfos.size());
                if (cret != 0) {
                    BM_LOG_INFO("client retry cleanup: batch close sockets return: " << cret);
                }
            }
        }
        BM_LOG_WARN("client connection attempt failed: " << ret << "; retrying in 1s");
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
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

int FixedRanksQpManager::WaitConnectionsReady(std::unordered_map<uint32_t, AiCoreConnChannel> &connections) noexcept
{
    /* count entries that are ALREADY settled on entry: the worker re-walks the whole map on
     * every retry attempt, and previously settled connections must satisfy the loop condition
     * from the start -- otherwise a settled map keeps issuing RaGetSockets on an EMPTY list,
     * which hccp rejects with 128203, and the attempt would fail spuriously */
    uint32_t totalSuccessCount = 0;
    for (auto it = connections.begin(); it != connections.end(); ++it) {
        if (it->second.socketFd[0] != nullptr) {
            totalSuccessCount++;
        }
    }
    auto start = std::chrono::steady_clock::now();
    auto timeout = start + std::chrono::seconds(15);
    while (totalSuccessCount < connections.size()) {
        if (started_.load() && !workersRunning_.load()) {
            /* workers were shut down (Shutdown path): stop waiting so CloseServices joins
             * fast. The plain started_ check keeps direct unit-test calls (no workers ever
             * started) waiting on the budget as before */
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
        if (socketInfos.empty()) {
            /* nothing left to query this round (settled by the base count above) */
            continue;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        auto role = (&connections == &clientConnections_) ? 1 : 0;
        auto ret = DlHccpApi::RaGetSockets(role, socketInfos.data(), socketInfos.size(), successCount);
        if (ret != 0) {
            if (ret == 128203) {
                /* hccp reports "no completed connection to hand out yet" as this code; in the
                 * retry model that is ordinary "no progress this poll", not an error */
                successCount = 0;
            } else {
                BM_LOG_ERROR("role(" << role << ") side get sockets failed: " << ret);
                return BM_DL_FUNCTION_FAILED;
            }
        }

        for (auto i = 0U; i < successCount; i++) {
            auto cret = CheckReadyConnection(connections, addr2index, socketInfos[i]);
            if (cret != BM_OK) {
                return cret;
            }
        }

        totalSuccessCount += successCount;
    }

    return BM_OK;
}

int FixedRanksQpManager::CreateQpWaitingReady(std::unordered_map<uint32_t, AiCoreConnChannel> &connections) noexcept
{
    const bool clientSide = (&connections == &clientConnections_);
    /* lane 0: legacy primary connections, one QP per rank; skip entries whose QP already
     * exists -- the worker re-walks the whole map on every retry attempt */
    for (auto it = connections.begin(); it != connections.end(); ++it) {
        if (it->second.qpHandle[0] != nullptr) {
            continue;
        }
        int ret = CreateOneQp(it->second, 0);
        if (ret != 0) {
            BM_LOG_ERROR("create QP  to " << it->first << " failed: " << ret);
            it->second.qpHandle[0] = nullptr; /* a failed create must not leave a stale handle */
            return BM_DL_FUNCTION_FAILED;
        }

        ret = DlHccpApi::RaQpConnectAsync(it->second.qpHandle[0], it->second.socketFd[0]);
        if (ret != 0) {
            BM_LOG_ERROR("connect AI QP to " << it->first << " failed: " << ret);
            /* destroy on the spot: keeping a connect-failed QP poisons every later poll
             * (observed: endless get-QP-status 328107 after connect 228102) */
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
        if (started_.load() && !workersRunning_.load()) {
            /* see WaitConnectionsReady: fast exit only when the workers were shut down */
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

int FixedRanksQpManager::FillQpInfo() noexcept
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
        /* snapshot under the lock: this runs on the group-event thread (SetRemoteRankInfo)
         * concurrently with the workers creating QPs / scanning members. A worker's QP fields
         * (aiQpInfo) it has not reported ready yet are read as stale zeros -- harmless, the
         * worker refreshes the table again once its QPs become ready */
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
    } /* end of the locked snapshot scope */

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
    /* stop the workers first so no thread touches the channel maps while we tear them down;
     * the running-flag checks inside their wait loops make the join fast */
    workersRunning_.store(false);
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
