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

#ifndef MF_HYBRID_DEVICE_RDMA_TRANSPORT_MANAGER_H
#define MF_HYBRID_DEVICE_RDMA_TRANSPORT_MANAGER_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <map>
#include <mutex>
#include <memory>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <unordered_map>
#include "hybm_define.h"
#include "hybm_stream_manager.h"
#include "hybm_stream_notify.h"
#include "hybm_transport_manager.h"
#include "device_chip_info.h"
#include "device_rdma_common.h"
#include "device_qp_manager.h"

namespace ock {
namespace mf {
namespace transport {
namespace device {

class RdmaTransportManager : public TransportManager {
public:
    ~RdmaTransportManager() override;
    Result OpenDevice(const TransportOptions &options) override;
    Result CloseDevice() override;
    Result RegisterMemoryRegion(const TransportMemoryRegion &mr) override;
    Result UnregisterMemoryRegion(uint64_t addr) override;
    bool QueryHasRegistered(uint64_t addr, uint64_t size) override;
    Result QueryMemoryKey(uint64_t addr, TransportMemoryKey &key) override;
    void UpdateMemoryKey(TransportMemoryKey &key, void *addr) override;
    Result Prepare(const HybmTransPrepareOptions &options) override;
    Result RemoveRanks(const std::vector<uint32_t> &removedRanks) override;
    Result Connect() override;
    Result AsyncConnect() override;
    Result WaitForConnected(int64_t timeoutNs) override;
    Result UpdateRankOptions(const HybmTransPrepareOptions &options) override;
    const std::string &GetNic() const override;
    const TransportPrivateData GetPrivateData() const override;
    const void *GetQpInfo() const override;
    Result ReadRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;
    Result WriteRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;
    Result ReadRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;
    Result WriteRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;
    Result Synchronize(uint32_t rankId) override;
    Result ReadRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor) override;
    Result WriteRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor) override;

private:
    static bool PrepareOpenDevice(uint32_t userId, uint32_t device, uint32_t rankCount, in_addr &deviceIp,
                                  void *&rdmaHandle);
    static bool OpenTsd(uint32_t deviceId, uint32_t rankCount);
    static bool RaInit(uint32_t deviceId);
    static bool RetireDeviceIp(uint32_t deviceId, in_addr &deviceIp);
    static bool RaRdevInit(uint32_t deviceId, in_addr deviceIp, void *&rdmaHandle);
    void ClearAllRegisterMRs();
    int CheckPrepareOptions(const HybmTransPrepareOptions &options);
    int RemoteIO(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size, bool write, bool sync);
    /* submit one stride-shard of a batch over the given lane QP: blocks begin, begin+lanes, ...
     * caller owns the qpHandle for the whole shard (one Get/Put per shard) */
    int RemoteIOLane(void *qpHandle, uint32_t rankId, const CopyDescriptor &descriptor, uint32_t lane,
                     uint32_t lanes, bool write);
    /* shard a batch across ready QP lanes: lane 0 runs on the calling thread, lanes 1..N-1
     * on the persistent worker pool; falls back to the serial per-block path when only one
     * lane is ready or the batch is small */
    int RemoteIOBatchSharded(uint32_t rankId, const CopyDescriptor &descriptor, bool write);
    void EnsureLaneWorkers(uint32_t lanes);
    void StopLaneWorkers();
    void LaneWorkerLoop(uint32_t workerIdx);
    int CorrectHostRegWr(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size, send_wr_v2 &wr);
    int ConvertHccpMrInfo(const TransportMemoryRegion &mr, HccpMrInfo &info);
    void OptionsToRankMRs(const HybmTransPrepareOptions &options);
    void SynthesizeSelfRankInfo(std::unordered_map<uint32_t, ConnectRankInfo> &rankInfo);
    Result WaitQpReady();
    int GetRegAddress(const MemoryRegionMap &map, uint64_t inputAddr, uint64_t size, bool isLocal, uint64_t &outputAddr,
                      uint32_t &mrKey) const;

private: // RDMA HOST STARS
    void ConstructSqeNoSinkModeForRdmaDbSendTask(const send_wr_rsp &rspInfo, rtStarsSqe_t &command, HybmStreamPtr st);
    uint64_t GetRoceDbAddrForRdmaDbSendTask();
    int32_t InitStreamNotifyBuf();
    int32_t Synchronize(void *qpHandle, uint32_t rankId);
    static void BuildTable(std::string &str, std::string &ip, uint32_t devId);
    static bool GetRdmaHandleAfterInitHccl(uint32_t device, in_addr &deviceIp, void *&rdmaHandle);

private:
    /* one dispatched batch, shared read-only by all lane workers; lane owned by worker i is
     * i+1, lane 0 belongs to the dispatching thread */
    struct LaneJob {
        uint32_t rankId{0};
        const CopyDescriptor *desc{nullptr};
        bool write{false};
        uint32_t lanes{1};
    };

    static thread_local HybmStreamNotifyPtr notify_;
    RdmaNotifyInfo notifyInfo_ = {};
    std::mutex mutex_;
    bool started_{false};
    uint32_t rankId_{0};
    uint32_t rankCount_{1};
    uint32_t deviceId_{0};
    uint32_t userDeviceId_{0};
    hybm_role_type role_{HYBM_ROLE_PEER};
    in_addr deviceIp_{0};
    uint16_t devicePort_{0};
    void *rdmaHandle_{nullptr};
    std::string nicInfo_;
    MemoryRegionMap registerMRS_; // key: hostVa, value: regMR
    std::vector<MemoryRegionMap> ranksMRs_;
    std::shared_ptr<DeviceQpManager> qpManager_;
    std::vector<std::pair<uint64_t, uint32_t>> notifyRemoteInfo_;
    std::shared_ptr<DeviceChipInfo> deviceChipInfo_;
    std::atomic<uint64_t> wrIdx_{0};

    ReadWriteLock lock_;

    /* persistent lane worker pool (host-plane batch sharding): workers are created lazily on
     * the first parallel batch and joined on CloseDevice/destruction; they must be persistent
     * because per-thread HybmStreams are cached by tid and would be re-allocated per spawn */
    std::vector<std::thread> laneWorkers_;
    /* single job slot: serializes the dispatch phase of concurrent batch callers (each
     * caller still runs its own lane 0 inside the lock) */
    std::mutex laneDispatchMutex_;
    std::mutex lanePoolMutex_;
    std::condition_variable laneWorkCv_; // wake workers on a new job / stop
    std::condition_variable laneDoneCv_; // dispatching thread waits for completion
    LaneJob laneJob_{};                  // guarded by lanePoolMutex_
    uint64_t laneJobId_{0};              // guarded by lanePoolMutex_
    bool lanePoolStop_{false};           // guarded by lanePoolMutex_
    std::atomic<uint32_t> lanePending_{0};
    std::atomic<int32_t> laneFirstErr_{0};
};
} // namespace device
} // namespace transport
} // namespace mf
} // namespace ock

#endif // MF_HYBRID_DEVICE_RDMA_TRANSPORT_MANAGER_H
