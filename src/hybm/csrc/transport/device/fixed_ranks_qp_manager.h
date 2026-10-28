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
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "device_qp_manager.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

class FixedRanksQpManager : public DeviceQpManager {
public:
    FixedRanksQpManager(uint32_t deviceId, uint32_t rankId, uint32_t rankCount, sockaddr_in devNet) noexcept;
    ~FixedRanksQpManager() noexcept override;

    int SetRemoteRankInfo(const std::unordered_map<uint32_t, ConnectRankInfo> &ranks) noexcept override;
    int Startup(void *rdma) noexcept override;
    void Shutdown() noexcept override;
    int WaitingConnectionReady() noexcept override;
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
    int StartServerSide() noexcept;
    int GenerateWhiteList() noexcept;
    /* build-once model: establishment is deferred to the "group full" point (member count ==
     * rankCount_), the last joiner's group event observed by every member. MaybeTriggerBuild
     * runs on the group-event thread (microseconds, never waits); BuildOnce owns whitelist ->
     * warm-up -> connect -> QP -> fill for the WHOLE member table in one background thread,
     * at most 3 rounds; WaitingConnectionReady gates user threads on the result */
    void MaybeTriggerBuild() noexcept;
    void BuildOnce() noexcept;
    bool LaunchClientConnects() noexcept;
    void CleanupRound() noexcept;
    int WaitConnectionsReady(std::unordered_map<uint32_t, AiCoreConnChannel> &connections) noexcept;
    int CheckReadyConnection(std::unordered_map<uint32_t, AiCoreConnChannel> &connections,
                             const std::unordered_map<in_addr_t, uint32_t> addr2index,
                             const HccpSocketInfo &socketInfo) noexcept;
    int CreateQpWaitingReady(std::unordered_map<uint32_t, AiCoreConnChannel> &connections) noexcept;
    int CreateOneQp(AiCoreConnChannel &channel, uint32_t lane) noexcept;
    /* strict=true (build-once path): a member without a connection is a real failure (ERROR +
     * abort). strict=false (SetRemoteRankInfo refresh): absent connections are the expected
     * pre-build transient -- log INFO, skip that rank's QP slots and keep filling the rest */
    int FillQpInfo(bool strict = true) noexcept;
    void CopyAiWQInfo(struct AiQpRMAWQ &dest, const struct ai_data_plane_wq &src, DBMode dbMode, uint32_t sl) noexcept;
    void CopyAiCQInfo(struct AiQpRMACQ &dest, const ai_data_plane_cq &source, DBMode dbMode) noexcept;
    void CloseServices() noexcept;
    void CloseClientConnections() noexcept;
    void CloseServerConnections() noexcept;
    void CloseConnections(std::unordered_map<uint32_t, AiCoreConnChannel> &connections) noexcept;

private:
    enum class BuildState { Idle, Building, Done, Failed };

    std::atomic<bool> started_{false};
    /* guards the STRUCTURE of currentRanksInfo_ and both connection maps across the
     * group-event thread (SetRemoteRankInfo / FillQpInfo) and the BuildOnce thread. The
     * BuildOnce wait/QP walks run lock-free: while building it is the only writer of the
     * connection maps; structural edits (emplace/erase) take the lock */
    mutable std::mutex mapsMutex_;
    uint32_t qpInfoSize_{0};
    uint32_t qpsPerPeer_{1};
    void *rdmaHandle_{nullptr};
    std::unordered_map<uint32_t, ConnectRankInfo> currentRanksInfo_;
    MemoryRegionMap currentLocalMrs_;
    AiQpRMAQueueInfo *qpInfo_{nullptr};
    std::mutex stateMutex_;
    std::condition_variable stateCv_;
    BuildState buildState_{BuildState::Idle};
    std::chrono::steady_clock::time_point lastBuildFailSteady_{};
    std::shared_ptr<std::thread> buildThread_;
    std::unordered_map<uint32_t, AiCoreConnChannel> clientConnections_;
    std::unordered_map<uint32_t, AiCoreConnChannel> serverConnections_;
};
} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock

#endif // MF_HYBRID_FIXED_RANKS_QP_MANAGER_H
