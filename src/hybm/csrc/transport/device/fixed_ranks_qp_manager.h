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

#ifndef MF_HYBRID_FIXED_RANKS_QP_MANAGER_H
#define MF_HYBRID_FIXED_RANKS_QP_MANAGER_H

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <thread>
#include "device_qp_manager.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

/* FixedRanksQpManager adopts the JoinableRanksQpManager establishment model:
 *
 *   SetRemoteRankInfo is O(1) bookkeeping (merge + classify into pending sets + notify) and
 *   NEVER waits on the network; two persistent run-loop threads own the actual connect /
 *   accept / QP work with per-round retries. This is a hard requirement, not a style choice:
 *   SetRemoteRankInfo runs inside the dynamic-group JoinHandle, whose barrier vote used to
 *   block on transport readiness -- coupled with the hccp whitelist race (a late joiner's
 *   connect firing microseconds before the peer's whitelist add) that produced the
 *   three-way circular wait observed on the FSDP cluster (join round stuck at
 *   "RA_(0)_D_5_1_GW resp -601" while the joiner's QPs sat status=3 forever).
 *
 * Readiness is enforced downstream by RdmaTransportManager::WaitQpReady polling
 * CheckQpReady() (ranks that already have memory regions), bounded at 30s + 100ms/rank --
 * same contract Joinable has always used. A timed-out import round simply retries; the
 * pending sets preserve progress, so no round can deadlock on another round's completion. */
class FixedRanksQpManager : public DeviceQpManager {
public:
    FixedRanksQpManager(uint32_t deviceId, uint32_t rankId, uint32_t rankCount, sockaddr_in devNet) noexcept;
    ~FixedRanksQpManager() noexcept override;

    int SetRemoteRankInfo(const std::unordered_map<uint32_t, ConnectRankInfo> &ranks) noexcept override;
    int Startup(void *rdma) noexcept override;
    void Shutdown() noexcept override;
    int WaitingConnectionReady() noexcept override;
    bool CheckQpReady(const std::vector<uint32_t> &rankIds) const noexcept override;
    const void *GetQpInfoAddress() const noexcept override;
    UserQpInfo *GetQpHandleWithRankId(uint32_t rankId) noexcept override;
    void PutQpHandle(UserQpInfo *qp) const noexcept override;

private:
    struct AiCoreConnChannel {
        in_addr remoteIp;
        void *socketHandle;
        void *socketFd[MAX_QP_LANES]{};
        void *qpHandle[MAX_QP_LANES]{};
        HccpAiQpInfo aiQpInfo[MAX_QP_LANES]{};
        int qpStatus[MAX_QP_LANES]{-1, -1, -1, -1};

        explicit AiCoreConnChannel(const in_addr ip) : AiCoreConnChannel{ip, nullptr} {}
        AiCoreConnChannel(in_addr ip, void *sock) : remoteIp{ip}, socketHandle{sock} {}
    };

    /* lane 0 is the legacy empty-tag primary connection, lanes 1..N-1 carry tagged
     * connections "mf_q2_<clientRank>_<lane>" multiplexing the same (ip, port) endpoint */
    static void MakeLaneTag(char *buf, size_t bufSize, uint32_t clientRank, uint32_t lane) noexcept;
    bool ReserveQpInfoSpace() noexcept;
    void ReleaseQpInfoSpace() noexcept;
    /* synchronous local setup only: listen socket + initial whitelist (server side) and
     * local socket-handle pre-creation (client side); no network waits belong in Startup */
    int StartServerSide() noexcept;
    int StartClientSide() noexcept;
    int GenerateWhiteList() noexcept;
    /* persistent establishment loops (started at Startup, woken by SetRemoteRankInfo):
     * each round serves the pending ranks of its direction; failures keep the rank
     * pending so the next round retries -- this is what heals the whitelist race */
    void ClientRunLoop() noexcept;
    void ServerRunLoop() noexcept;
    void ClientSideCatchUp(const std::set<uint32_t> &targets) noexcept;
    void ServerSideCatchUp(const std::set<uint32_t> &targets) noexcept;
    bool RankMeshReady(uint32_t rankId) const noexcept;
    /* roundBudgetSec bounds one establishment round (see ClientSideCatchUp): short waits +
     * retries heal races, long handler-stack waits deadlocked the group protocol */
    static uint32_t RoundBudgetSec() noexcept;
    int WaitConnectionsReady(std::unordered_map<uint32_t, AiCoreConnChannel> &connections,
                             uint32_t roundBudgetSec) noexcept;
    int CheckReadyConnection(std::unordered_map<uint32_t, AiCoreConnChannel> &connections,
                             const std::unordered_map<in_addr_t, uint32_t> addr2index,
                             const HccpSocketInfo &socketInfo) noexcept;
    int CreateQpWaitingReady(std::unordered_map<uint32_t, AiCoreConnChannel> &connections,
                             uint32_t roundBudgetSec) noexcept;
    int CreateOneQp(AiCoreConnChannel &channel, uint32_t lane) noexcept;
    int FillQpInfo() noexcept;
    void CopyAiWQInfo(struct AiQpRMAWQ &dest, const struct ai_data_plane_wq &src, DBMode dbMode, uint32_t sl) noexcept;
    void CopyAiCQInfo(struct AiQpRMACQ &dest, const ai_data_plane_cq &source, DBMode dbMode) noexcept;
    void CloseServices() noexcept;
    void CloseClientConnections() noexcept;
    void CloseServerConnections() noexcept;
    void CloseConnections(std::unordered_map<uint32_t, AiCoreConnChannel> &connections) noexcept;

private:
    std::atomic<bool> started_{false};
    std::atomic<int> serverConnectResult{-1};
    std::atomic<int> clientConnectResult{-1};
    uint32_t qpInfoSize_{0};
    uint32_t qpsPerPeer_{1};
    void *rdmaHandle_{nullptr};
    std::unordered_map<uint32_t, ConnectRankInfo> currentRanksInfo_;
    MemoryRegionMap currentLocalMrs_;
    AiQpRMAQueueInfo *qpInfo_{nullptr};
    /* loop-thread state: pending ranks that still need client-connect / server-accept
     * work; a rank leaves its set only after its channel is fully established */
    std::atomic<bool> running_{false};
    std::mutex loopMutex_;
    std::condition_variable loopCv_;
    std::set<uint32_t> pendingClientRanks_;
    std::set<uint32_t> pendingServerRanks_;
    /* structure (emplace/erase/clear) of the two channel maps is mutated by the owning
     * worker only, under a unique lock; FillQpInfo/CheckQpReady read under a shared lock.
     * In-place field publishes (socketFd/qpHandle/qpStatus/aiQpInfo) are plain writes by
     * the owning worker -- readers tolerate transiently stale lanes, same as Joinable. */
    mutable std::shared_mutex channelsMutex_;
    std::shared_ptr<std::thread> clientConnectThread_;
    std::shared_ptr<std::thread> serverConnectThread_;
    std::unordered_map<uint32_t, AiCoreConnChannel> clientConnections_;
    std::unordered_map<uint32_t, AiCoreConnChannel> serverConnections_;
};
} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock

#endif // MF_HYBRID_FIXED_RANKS_QP_MANAGER_H
