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
#include "hybm_logger.h"
#include "dl_hccp_api.h"
#include "dl_acl_api.h"
#include "joinable_ranks_qp_manager.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {
constexpr int MR_INFO_ACCESS = 7;
constexpr int WAIT_TIME_MS = 300;

void JoinableRanksQpManager::MakeLaneTag(char *buf, size_t bufSize, uint32_t clientRank, uint32_t lane) noexcept
{
    /* "mf_hl_" prefix keeps host-plane tags distinct from the device plane's "mf_q2_" tags;
     * derived from the CLIENT's rank so both ends derive the same string; lane 0 stays on
     * the legacy empty-tag primary connection */
    snprintf(buf, bufSize, "mf_hl_%u_%u", clientRank, lane);
}

JoinableRanksQpManager::JoinableRanksQpManager(uint32_t userDeviceId, uint32_t deviceId, uint32_t rankId,
                                               uint32_t rankCount, sockaddr_in devNet) noexcept
    : DeviceQpManager(deviceId, rankId, rankCount, devNet, HYBM_ROLE_PEER)
{
    connections_.resize(rankCount);
    qpArray_.resize(rankCount, {});
    userDeviceId_ = userDeviceId;
}

JoinableRanksQpManager::~JoinableRanksQpManager() noexcept
{
    CloseServices();
}

int JoinableRanksQpManager::SetRemoteRankInfo(const std::unordered_map<uint32_t, ConnectRankInfo> &ranks) noexcept
{
    std::unique_lock<std::mutex> uniqueLock{mutex_};
    for (auto it = ranks.begin(); it != ranks.end(); ++it) {
        if (it->first >= rankCount_) {
            continue;
        }
        connections_[it->first].remoteNet = it->second.network;
        if (it->first < rankId_) {
            newServers_.emplace(it->first);
        }
        if (it->first > rankId_) {
            newClients_.emplace(it->first);
        }
    }
    uniqueLock.unlock();

    if (started_.load()) {
        cond_.notify_all();
    }

    return BM_OK;
}

// 上层保证 SetRemoteRankInfo和RemoveRanks操作不并发
int JoinableRanksQpManager::RemoveRanks(const std::unordered_set<uint32_t> &ranks) noexcept
{
    std::unique_lock<std::mutex> uniqueLock{mutex_};
    for (auto rank : ranks) {
        if (rank < rankId_) {
            removedServerRanks_.emplace(rank);
        } else {
            removedClientRanks_.emplace(rank);
        }
    }
    uniqueLock.unlock();

    if (started_.load()) {
        cond_.notify_all();
    }

    return BM_OK;
}

int JoinableRanksQpManager::Startup(void *rdma) noexcept
{
    if (rdma == nullptr) {
        BM_LOG_ERROR("input rdma is null");
        return BM_INVALID_PARAM;
    }
    std::unique_lock<std::mutex> unique_lock(mutex_);
    if (started_.load()) {
        BM_LOG_DEBUG("already started.");
        return BM_OK;
    }

    rdmaHandle_ = rdma;
    qpsPerPeer_ = ResolveQpsPerPeer();
    if (qpsPerPeer_ > 1U) {
        BM_LOG_INFO("host-plane multi-QP enabled: " << qpsPerPeer_ << " QP lanes per peer (tag multiplexing)");
    }
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

    started_.store(true);
    return BM_OK;
}

void JoinableRanksQpManager::Shutdown() noexcept
{
    std::unique_lock<std::mutex> unique_lock(mutex_);
    std::set<uint32_t> list;
    for (uint32_t i = 0; i < connections_.size(); ++i) {
        list.insert(i);
    }
    RemoveRanksProcess(list);
    started_.store(false);
    running_.store(false);
    rdmaHandle_ = nullptr;
    qpArray_.clear();
    connections_.clear();
    newClients_.clear();
    newServers_.clear();
    removedClientRanks_.clear();
    removedServerRanks_.clear();
    unique_lock.unlock();
    CloseServices();
}

UserQpInfo *JoinableRanksQpManager::GetQpHandleWithRankId(uint32_t rankId) noexcept
{
    if (rankId >= rankCount_) {
        BM_LOG_ERROR("invalid rank id: " << rankId << ", rank count: " << rankCount_);
        return nullptr;
    }
    ReadGuard lockGuard(qpLock_);
    if (qpArray_[rankId][0] != nullptr) {
        qpArray_[rankId][0]->ref.fetch_add(1U);
        return qpArray_[rankId][0];
    }
    return nullptr;
}

UserQpInfo *JoinableRanksQpManager::GetLaneQpHandleWithRankId(uint32_t rankId, uint32_t lane) noexcept
{
    if (rankId >= rankCount_ || lane >= qpsPerPeer_) {
        return nullptr;
    }
    ReadGuard lockGuard(qpLock_);
    if (qpArray_[rankId][lane] != nullptr) {
        qpArray_[rankId][lane]->ref.fetch_add(1U);
        return qpArray_[rankId][lane];
    }
    return nullptr;
}

uint32_t JoinableRanksQpManager::GetQpLaneCount(uint32_t rankId) noexcept
{
    if (rankId >= rankCount_ || qpsPerPeer_ <= 1U) {
        return 1U;
    }
    for (uint32_t lane = 0; lane < qpsPerPeer_; lane++) {
        if (connections_[rankId].qpStatus[lane] != 1) {
            return 1U;
        }
    }
    return qpsPerPeer_;
}

void JoinableRanksQpManager::PutQpHandle(UserQpInfo *qp) const noexcept
{
    uint32_t val = qp->ref.fetch_sub(1U);
    if (val == 1U) { // 返回减之前的值
        auto ret = DlHccpApi::RaQpDestroy(qp->qpHandle);
        if (ret != 0) {
            BM_LOG_WARN("close qp from " << rankId_ << " failed, ret: " << ret);
        }
        delete qp;
    }
}

bool JoinableRanksQpManager::CheckQpReady(const std::vector<uint32_t> &rankIds) const noexcept
{
    for (auto rankId : rankIds) {
        if (rankId == rankId_) {
            continue;
        }
        if (rankId >= rankCount_) {
            BM_LOG_ERROR("invalid rank id: " << rankId << ", rank count: " << rankCount_);
            return false;
        }
        for (uint32_t lane = 0; lane < qpsPerPeer_; lane++) {
            if (connections_[rankId].qpStatus[lane] != 1) {
                return false;
            }
        }
    }
    return true;
}

void JoinableRanksQpManager::CloseServices() noexcept
{
    running_.store(false);
    cond_.notify_all();
    if (serverConnectThread_ != nullptr) {
        serverConnectThread_->join();
        serverConnectThread_ = nullptr;
    }
    if (clientConnectThread_ != nullptr) {
        clientConnectThread_->join();
        clientConnectThread_ = nullptr;
    }
}

int JoinableRanksQpManager::StartServerSide() noexcept
{
    if (rankId_ + 1U == rankCount_) {
        return BM_OK;
    }

    auto ret = CreateServerSocket();
    if (ret != BM_OK) {
        BM_LOG_ERROR("create server socket failed: " << ret);
        return ret;
    }

    serverConnectThread_ = std::make_shared<std::thread>([this]() { ServerSideRunLoop(); });
    return BM_OK;
}

int JoinableRanksQpManager::StartClientSide() noexcept
{
    if (rankId_ > 0U) {
        clientConnectThread_ = std::make_shared<std::thread>([this]() { ClientSideRunLoop(); });
    }
    return BM_OK;
}

void JoinableRanksQpManager::ServerSideHandleNewClients(const std::set<uint32_t> &newRanks) noexcept
{
    auto ret = GenerateWhiteList(newRanks);
    if (ret != 0) {
        BM_LOG_ERROR("generate white list failed: " << ret);
        return;
    }

    ret = WaitSocketConnections(newRanks);
    if (ret != 0) {
        BM_LOG_ERROR("make socket connections for server side failed: " << ret);
        return;
    }

    MakeQpConnections(newRanks);
    WaitQpConnections(newRanks);
}

void JoinableRanksQpManager::ServerSideRunLoop() noexcept
{
    DlAclApi::AclrtSetDevice(userDeviceId_);

    while (running_) {
        std::unique_lock<std::mutex> uniqueLock{mutex_};
        cond_.wait_for(uniqueLock, std::chrono::milliseconds(WAIT_TIME_MS));
        if (newClients_.empty() && removedClientRanks_.empty() && running_) {
            cond_.wait_for(uniqueLock, std::chrono::minutes(1));
        }
        if (!running_) {
            break;
        }
        auto newClients = newClients_;
        auto removedRanks = std::move(removedClientRanks_);
        uniqueLock.unlock();

        if (!newClients.empty()) {
            ServerSideHandleNewClients(newClients);
        }

        if (!removedRanks.empty()) {
            RemoveRanksProcess(removedRanks);
        }
    }
}

void JoinableRanksQpManager::ClientSideRunLoop() noexcept
{
    DlAclApi::AclrtSetDevice(userDeviceId_);
    while (running_) {
        std::unique_lock<std::mutex> uniqueLock{mutex_};
        cond_.wait_for(uniqueLock, std::chrono::milliseconds(WAIT_TIME_MS));
        if (newServers_.empty() && removedServerRanks_.empty() && running_) {
            cond_.wait_for(uniqueLock, std::chrono::minutes(1));
        }
        if (!running_) {
            break;
        }
        auto newServers = newServers_;
        auto removedRanks = std::move(removedServerRanks_);
        uniqueLock.unlock();

        if (!newServers.empty()) {
            auto ret = CreateConnectionToServers(newServers);
            if (ret != 0) {
                BM_LOG_ERROR("create connection to server failed: " << ret);
            }

            WaitSocketConnections(newServers);
            MakeQpConnections(newServers);
            WaitQpConnections(newServers);
        }

        if (!removedRanks.empty()) {
            RemoveRanksProcess(removedRanks);
        }
    }
}

int JoinableRanksQpManager::WaitSocketConnections(const std::set<uint32_t> &newRanks) noexcept
{
    if (newRanks.empty()) {
        return BM_OK;
    }

    bool socketRole = *newRanks.begin() < rankId_ ? 1 : 0;
    std::vector<HccpSocketInfo> socketInfos;
    std::unordered_map<in_addr_t, uint32_t> addr2rank;
    for (auto rankId : newRanks) {
        if (connections_[rankId].remoteNet.sin_addr.s_addr == 0) {
            BM_LOG_ERROR("rankId: " << rankId << ", no ip address.");
            continue;
        }

        if (connections_[rankId].socketFd[0] != nullptr) {
            continue;
        }

        HccpSocketInfo info{};
        info.handle = connections_[rankId].socketHandle;
        info.fd = nullptr;
        info.remoteIp.addr = connections_[rankId].remoteNet.sin_addr;
        info.status = 0;
        bzero(info.tag, sizeof(info.tag));
        socketInfos.push_back(info);
        addr2rank.emplace(info.remoteIp.addr.s_addr, rankId);
    }

    if (socketInfos.empty()) {
        return BM_OK;
    }

    auto startTime = std::chrono::steady_clock::now();
    constexpr auto MAX_WAIT_DURATION = std::chrono::hours(2);
    uint32_t batchCnt = 16U;
    do {
        uint32_t cnt = 0;
        uint32_t getSize = socketInfos.size() < batchCnt ? socketInfos.size() : batchCnt;
        auto ret = DlHccpApi::RaGetSockets(socketRole, socketInfos.data(), getSize, cnt);
        if (ret != 0) {
            BM_LOG_ERROR("socketRole(" << socketRole << ") side get sockets failed: " << ret);
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100L));
        if (std::chrono::steady_clock::now() - startTime > MAX_WAIT_DURATION) {
            BM_LOG_ERROR("GetSocketFdByAddrs timeout after 2 hours, remaining sockets: " << socketInfos.size());
            return 1;
        }
        if (cnt == 0) {
            continue;
        }
        for (auto i = 0U; i < getSize; i++) {
            if (socketInfos[i].status != 1) {
                continue;
            }
            if (qpsPerPeer_ > 1U && socketInfos[i].tag[0] != '\0') {
                /* a tagged lane accept surfaced in the empty-tag primary polling; lane fds
                 * are fetched by their own tag-scoped queries below -- keep polling for
                 * the primary */
                BM_LOG_INFO("multi-QP: tagged accept surfaced in primary polling, tag: '"
                              << socketInfos[i].tag << "', keep waiting primary.");
                socketInfos[i].status = 0; // not claimed: stay in the pending list
                continue;
            }
            auto socketInfoPos = addr2rank.find(socketInfos[i].remoteIp.addr.s_addr);
            if (socketInfoPos == addr2rank.end()) {
                BM_LOG_ERROR("socket ip(" << DescribeIPv4(socketInfos[i].remoteIp.addr) << ") should not exist.");
                continue;
            }
            auto rankId = socketInfoPos->second;
            if (rankId >= rankCount_) {
                BM_LOG_ERROR("socket ip(" << DescribeIPv4(socketInfos[i].remoteIp.addr) << ") should not exist.");
                continue;
            }
            if (connections_[rankId].socketFd[0] != nullptr) {
                BM_LOG_ERROR("get ip(" << DescribeIPv4(socketInfos[i].remoteIp.addr) << ") already get socket fd.");
                continue;
            }
            connections_[rankId].socketFd[0] = socketInfos[i].fd;
        }
        std::vector<HccpSocketInfo>::iterator it = socketInfos.begin();
        for (; it != socketInfos.end();) {
            if (it->status == 1) {
                it = socketInfos.erase(it);
            } else {
                it++;
            }
        }
    } while (socketInfos.size() > 0);

    if (qpsPerPeer_ > 1U) {
        /* extra tagged lanes: fetch each lane fd by its tag-scoped query, all-or-nothing --
         * a dead lane tears the whole channel down so the retry round rebuilds it */
        for (auto rankId : newRanks) {
            if (connections_[rankId].socketFd[0] == nullptr) {
                continue; // primary not up this round; lanes retry with it later
            }
            const uint32_t role = (rankId < rankId_) ? 1U : 0U;
            const uint32_t clientRank = (rankId < rankId_) ? rankId_ : rankId;
            for (uint32_t lane = 1; lane < qpsPerPeer_; lane++) {
                if (connections_[rankId].socketFd[lane] != nullptr) {
                    continue;
                }
                char tag[HCCP_SOCK_CONN_TAG_SIZE];
                MakeLaneTag(tag, sizeof(tag), clientRank, lane);
                void *laneFd = nullptr;
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                while (std::chrono::steady_clock::now() < deadline) {
                    HccpSocketInfo info{};
                    info.handle = connections_[rankId].socketHandle;
                    info.fd = nullptr;
                    info.remoteIp.addr = connections_[rankId].remoteNet.sin_addr;
                    info.status = 0;
                    snprintf(info.tag, sizeof(info.tag), "%s", tag);
                    uint32_t cnt = 0;
                    auto gret = DlHccpApi::RaGetSockets(role, &info, 1U, cnt);
                    if (gret == 0 && cnt > 0 && info.status == 1 && info.fd != nullptr &&
                        info.fd != connections_[rankId].socketFd[0]) {
                        laneFd = info.fd;
                        break;
                    }
                    if (gret == 0 && cnt > 0 && info.status == 2) {
                        BM_LOG_ERROR("multi-QP lane " << lane << " to rank " << rankId
                                                        << " connect rejected/timeout (tag '" << tag << "')");
                        TeardownRankConnection(rankId);
                        return 1;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                if (laneFd == nullptr) {
                    BM_LOG_ERROR("multi-QP lane " << lane << " to rank " << rankId
                                                    << " no fd within deadline (tag '" << tag << "')");
                    TeardownRankConnection(rankId);
                    return 1;
                }
                connections_[rankId].socketFd[lane] = laneFd;
                BM_LOG_INFO("multi-QP lane " << lane << " to rank " << rankId << " fd " << laneFd
                                                << " tag '" << tag << "'");
            }
        }
    }
    return BM_OK;
}

void JoinableRanksQpManager::MakeQpConnections(const std::set<uint32_t> &newRanks) noexcept
{
    if (newRanks.empty()) {
        return;
    }

    for (auto rankId : newRanks) {
        if (connections_[rankId].socketFd[0] == nullptr) {
            continue;
        }

        bool rankOk = true;
        for (uint32_t lane = 0; lane < qpsPerPeer_; lane++) {
            if (connections_[rankId].socketFd[lane] == nullptr) {
                BM_LOG_ERROR("rank " << rankId << " lane " << lane << " socket fd missing");
                rankOk = false;
                break;
            }

            if (connections_[rankId].qpHandle[lane] == nullptr) {
                void *qpHandle = nullptr;
                auto info = new (std::nothrow) UserQpInfo;
                BM_ASSERT_RET_VOID(info != nullptr);
                auto ret = DlHccpApi::RaQpCreate(rdmaHandle_, 0, 4, qpHandle);
                if (ret != 0) {
                    BM_LOG_ERROR("create QP lane " << lane << " to " << rankId << " failed: " << ret);
                    delete info;
                    rankOk = false;
                    break;
                }

                connections_[rankId].qpHandle[lane] = qpHandle;
                connections_[rankId].qpConnectCalled[lane] = false;

                info->ref.store(1U);
                info->qpHandle = qpHandle;
                WriteGuard lockGuard(qpLock_);
                qpArray_[rankId][lane] = info;
            }

            if (!connections_[rankId].qpConnectCalled[lane]) {
                auto ret = DlHccpApi::RaQpConnectAsync(connections_[rankId].qpHandle[lane],
                                                       connections_[rankId].socketFd[lane]);
                if (ret != 0) {
                    BM_LOG_ERROR("create QP lane " << lane << " from " << rankId_ << " to " << rankId
                                                      << " failed: " << ret);
                    rankOk = false;
                    break;
                }
                BM_LOG_INFO("create QP lane " << lane << " success from " << rankId_ << " to " << rankId);
                connections_[rankId].qpConnectCalled[lane] = true;
            }
        }

        if (!rankOk) {
            /* all-or-nothing: drop the partial channel so the next retry round rebuilds the
             * whole rank connection (rank stays in newServers_/newClients_) */
            TeardownRankConnection(rankId);
        }
    }
}

void JoinableRanksQpManager::WaitQpConnections(const std::set<uint32_t> &newRanks) noexcept
{
    if (newRanks.empty()) {
        return;
    }

    std::set<uint32_t> finishedRanks;
    for (auto rankId : newRanks) {
        if (connections_[rankId].qpHandle[0] == nullptr || !connections_[rankId].qpConnectCalled[0]) {
            continue;
        }

        bool rankReady = true;
        for (uint32_t lane = 0; lane < qpsPerPeer_; lane++) {
            if (connections_[rankId].qpHandle[lane] == nullptr || !connections_[rankId].qpConnectCalled[lane]) {
                rankReady = false;
                break;
            }
            if (connections_[rankId].qpStatus[lane] == 1) {
                continue;
            }
            auto ret = DlHccpApi::RaGetQpStatus(connections_[rankId].qpHandle[lane],
                                                connections_[rankId].qpStatus[lane]);
            if (ret != 0) {
                BM_LOG_ERROR("get QP status lane " << lane << " to " << rankId << " failed: " << ret);
                rankReady = false;
                break;
            }
            if (connections_[rankId].qpStatus[lane] != 1) {
                rankReady = false;
            }
        }

        if (rankReady) {
            BM_LOG_INFO("from " << rankId_ << " to " << rankId << " all " << qpsPerPeer_ << " qp lane(s) ready.");
            finishedRanks.emplace(rankId);
        }
    }

    std::unique_lock<std::mutex> uniqueLock{mutex_};
    for (auto rankId : finishedRanks) {
        if (rankId < rankId_) {
            newServers_.erase(rankId);
        } else {
            newClients_.erase(rankId);
        }
    }
}

int JoinableRanksQpManager::GenerateWhiteList(const std::set<uint32_t> &newClients) noexcept
{
    std::vector<HccpSocketWhiteListInfo> whitelist;
    for (auto rankId : newClients) {
        if (rankId <= rankId_ || rankId >= rankCount_) {
            BM_LOG_ERROR("new client rankId: " << rankId << "invalid. self: " << rankId_ << ", total: " << rankCount_);
            return BM_ERROR;
        }

        if (connections_[rankId].remoteNet.sin_addr.s_addr == 0) {
            BM_LOG_ERROR("rankId: " << rankId << ", no ip address.");
            return BM_ERROR;
        }

        if (connections_[rankId].socketHandle != nullptr) {
            continue;
        }

        HccpSocketWhiteListInfo info{};
        info.remoteIp.addr = connections_[rankId].remoteNet.sin_addr;
        info.connLimit = rankCount_;
        bzero(info.tag, sizeof(info.tag));
        whitelist.emplace_back(info);
        if (qpsPerPeer_ > 1U) {
            /* whitelist the peer's tagged lanes up front, before any client connects; the
             * peer is the connecting client, so the tag derives from the remote rank */
            for (uint32_t lane = 1; lane < qpsPerPeer_; lane++) {
                HccpSocketWhiteListInfo extra{};
                extra.remoteIp.addr = connections_[rankId].remoteNet.sin_addr;
                extra.connLimit = rankCount_;
                MakeLaneTag(extra.tag, sizeof(extra.tag), rankId, lane);
                whitelist.emplace_back(extra);
                BM_LOG_INFO("multi-QP server: lane " << lane << " whitelist rank " << rankId
                                                          << " tag '" << extra.tag << "'");
            }
        }
        connections_[rankId].socketHandle = serverSocketHandle_;
    }

    if (whitelist.empty()) {
        return BM_OK;
    }

    uint32_t batchSize = 16;
    for (size_t i = 0; i < whitelist.size(); i += batchSize) {
        size_t currentBatchSize = (whitelist.size() - i) >= batchSize ? batchSize : (whitelist.size() - i);
        auto batchStart = whitelist.begin() + i;
        auto batchEnd = batchStart + currentBatchSize;
        std::vector<HccpSocketWhiteListInfo> currentBatch(batchStart, batchEnd);
        auto ret = DlHccpApi::RaSocketWhiteListAdd(serverSocketHandle_, currentBatch.data(), currentBatch.size());
        if (ret != 0) {
            BM_LOG_ERROR("RaSocketWhiteListAdd() with size=" << currentBatch.size() << " failed: " << ret);
            return BM_ERROR;
        }
    }

    return BM_OK;
}

int JoinableRanksQpManager::CreateConnectionToServers(const std::set<uint32_t> &newServers) noexcept
{
    std::vector<HccpSocketConnectInfo> connectInfos;
    std::set<uint32_t> rollbacks;
    for (auto rankId : newServers) {
        if (rankId >= rankId_) {
            BM_LOG_ERROR("new server rankId: " << rankId << "invalid. self: " << rankId_);
            return BM_ERROR;
        }

        if (connections_[rankId].remoteNet.sin_addr.s_addr == 0) {
            BM_LOG_ERROR("rankId: " << rankId << ", no ip address.");
            return BM_ERROR;
        }

        if (connections_[rankId].socketHandle != nullptr) {
            continue;
        }

        auto socketHandle = CreateLocalSocket();
        if (socketHandle == nullptr) {
            BM_LOG_ERROR("create local socket to connect to server: " << rankId << " failed");
            return BM_ERROR;
        }
        connections_[rankId].socketHandle = socketHandle;
        rollbacks.emplace(rankId);

        HccpSocketConnectInfo connectInfo;
        connectInfo.handle = socketHandle;
        connectInfo.remoteIp.addr = connections_[rankId].remoteNet.sin_addr;
        connectInfo.port = connections_[rankId].remoteNet.sin_port;
        bzero(connectInfo.tag, sizeof(connectInfo.tag));
        BM_LOG_INFO("add connecting server " << connectInfo);
        connectInfos.emplace_back(connectInfo);
        if (qpsPerPeer_ > 1U) {
            /* extra tagged lanes ride the SAME (ip, port) endpoint and the SAME local handle:
             * hccp multiplexes connections by tag; we are the client, so the tag derives
             * from our own rank */
            for (uint32_t lane = 1; lane < qpsPerPeer_; lane++) {
                HccpSocketConnectInfo extra = connectInfo;
                MakeLaneTag(extra.tag, sizeof(extra.tag), rankId_, lane);
                connectInfos.emplace_back(extra);
                BM_LOG_INFO("multi-QP client: lane " << lane << " entry to rank " << rankId
                                                          << " tag '" << extra.tag << "'");
            }
        }
    }

    if (connectInfos.empty()) {
        return BM_OK;
    }
    uint32_t batchSize = 16;
    for (size_t i = 0; i < connectInfos.size(); i += batchSize) {
        size_t currentBatchSize = (connectInfos.size() - i) >= batchSize ? batchSize : (connectInfos.size() - i);
        auto batchStart = connectInfos.begin() + i;
        auto batchEnd = batchStart + currentBatchSize;
        std::vector<HccpSocketConnectInfo> currentBatch(batchStart, batchEnd);

        auto ret = DlHccpApi::RaSocketBatchConnect(currentBatch.data(), currentBatch.size());
        if (ret != 0) {
            BM_LOG_ERROR("connect to all servers failed: " << ret << ", servers count = " << connectInfos.size());
            return BM_ERROR;
        }
    }
    return BM_OK;
}

void JoinableRanksQpManager::TeardownRankConnection(uint32_t rankId) noexcept
{
    if (rankId >= connections_.size()) {
        return;
    }
    auto &channel = connections_[rankId];

    for (uint32_t lane = 0; lane < MAX_QP_LANES; lane++) {
        if (channel.qpHandle[lane] == nullptr) {
            continue;
        }
        UserQpInfo *info = nullptr;
        {
            WriteGuard guard(qpLock_);
            info = qpArray_[rankId][lane];
            qpArray_[rankId][lane] = nullptr;
        }
        channel.qpHandle[lane] = nullptr;
        channel.qpConnectCalled[lane] = false;
        channel.qpStatus[lane] = -1;
        if (info != nullptr) {
            PutQpHandle(info);
        }
    }

    std::vector<HccpSocketCloseInfo> closeInfos;
    for (uint32_t lane = 0; lane < MAX_QP_LANES; lane++) {
        if (channel.socketFd[lane] != nullptr) {
            HccpSocketCloseInfo closeInfo{};
            closeInfo.handle = channel.socketHandle;
            closeInfo.fd = channel.socketFd[lane];
            closeInfo.linger = 0;
            closeInfos.emplace_back(closeInfo);
            channel.socketFd[lane] = nullptr;
        }
    }
    if (!closeInfos.empty()) {
        auto ret = DlHccpApi::RaSocketBatchClose(closeInfos.data(), closeInfos.size());
        if (ret != 0) {
            BM_LOG_WARN("close sockets to rank " << rankId << " failed: " << ret);
        } else {
            BM_LOG_INFO("close " << closeInfos.size() << " socket(s) to rank " << rankId << " successful.");
        }
    }

    if (channel.socketHandle != nullptr) {
        if (channel.socketHandle == serverSocketHandle_ && serverSocketHandle_ != nullptr) {
            /* server channels borrow the shared listen handle; drop this rank's whitelist
             * entries (lane 0 empty tag + tagged lanes) so a retry round re-adds them
             * cleanly instead of duplicating */
            std::vector<HccpSocketWhiteListInfo> entries;
            HccpSocketWhiteListInfo primary{};
            primary.remoteIp.addr = channel.remoteNet.sin_addr;
            primary.connLimit = rankCount_;
            entries.emplace_back(primary);
            for (uint32_t lane = 1; lane < qpsPerPeer_; lane++) {
                HccpSocketWhiteListInfo extra{};
                extra.remoteIp.addr = channel.remoteNet.sin_addr;
                extra.connLimit = rankCount_;
                MakeLaneTag(extra.tag, sizeof(extra.tag), rankId, lane);
                entries.emplace_back(extra);
            }
            auto ret = DlHccpApi::RaSocketWhiteListDel(serverSocketHandle_, entries.data(), entries.size());
            if (ret != 0) {
                BM_LOG_INFO("whitelist del for rank " << rankId << " return: " << ret);
            }
        } else if (channel.socketHandle != serverSocketHandle_) {
            auto ret = DlHccpApi::RaSocketDeinit(channel.socketHandle);
            if (ret != 0) {
                BM_LOG_INFO("deinit socket to rank " << rankId << " return: " << ret);
            }
        }
        channel.socketHandle = nullptr;
    }

    bzero(&channel, sizeof(ConnectionChannel));
}

void JoinableRanksQpManager::RemoveRanksProcess(const std::set<uint32_t> &ranks) noexcept
{
    for (auto rank : ranks) {
        if (rank >= connections_.size() || rank == rankId_) {
            continue;
        }
        if (connections_[rank].socketHandle == nullptr && connections_[rank].socketFd[0] == nullptr) {
            continue;
        }
        BM_LOG_INFO("close connection from " << rankId_ << " to " << rank);
        TeardownRankConnection(rank);
    }
}

} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock