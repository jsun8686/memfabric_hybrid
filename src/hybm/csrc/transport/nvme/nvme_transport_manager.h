/*
 * Copyright: (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#ifndef MF_HYBRID_NVME_TRANSPORT_MANAGER_H
#define MF_HYBRID_NVME_TRANSPORT_MANAGER_H

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "hybm_transport_manager.h"
#include "hybm_nvme_segment.h"

namespace ock {
namespace mf {
namespace transport {

/* NVMe-oF transport: the third sub-transport of ComposeTransportManager, aligned with
 * the device/host sub-transports. Owns the whole NVMe transmission domain that used to
 * live inside HostDataOpNvme:
 *   - SPDK/EAL process environment (OpenDevice = EnsureSpdkEnv, once-per-success)
 *   - ctrlr connection table keyed by nqn|traddr|trsvcid|nsid, shared process-wide
 *     (multiple pool entities reuse the same target connection)
 *   - qpair slot pools per ctrlr (try-lock rotation lease, no global IO lock)
 *   - staging lazy pools (chunk blocks + 512B sectors, spdk_dma_malloc domain)
 *   - the user-memory registration table (MR domain, spdk_mem_register zero-copy)
 *   - LBA protocol adaptation: MDTS splitting, sub-sector RMW, window engine
 * rank semantics: an NVMe extent belongs to an owner rank (extent.ownerRank) and the
 * transport addresses its target through the extent -- ReadRemote(rankId, lAddr,
 * rAddr=GVA, size) resolves the GVA to the extent and verifies the owner. */
class NvmeTransportManager : public TransportManager {
public:
    /* process-wide singleton: SPDK env and ctrlr connections are shared across ALL
     * pool entities (same shape as HcomTransportManager::GetInstance) */
    static std::shared_ptr<NvmeTransportManager> GetInstance() noexcept;
    /* destroy the singleton (UT isolation only: no in-flight IO may exist) */
    static void ResetInstanceForTest() noexcept;

    /* one copy item of the NVMe data plane (operator -> transport) */
    struct NvmeCopyItem {
        char *local{nullptr};
        uint64_t gva{0};
        uint64_t size{0};
    };

    /* ---- TransportManager interface ---- */
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
    Result ReadRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;
    Result WriteRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;
    Result ReadRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;
    Result WriteRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size) override;
    Result Synchronize(uint32_t rankId) override;
    Result WriteRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor) override;
    Result ReadRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor) override;

    /* ---- NVMe domain API (operator-facing) ---- */
    /* idempotent SPDK env bring-up (dlopen + EAL + post_init); pool-create time A8a
     * hard-fail semantics are driven by the operator calling this from Initialize */
    Result EnsureEnv() noexcept;
    /* connect the ctrlr of one extent + pre-create its qpair slots (idempotent);
     * fired via the extent-registry warmup hook, HARD FAIL on unreachable target */
    Result Warmup(const HybmNvmeExtent &extent, const std::string &trstring) noexcept;
    /* run a whole batch inside per-endpoint windows (grouping + MDTS + RMW + zc) */
    Result RunItems(std::vector<NvmeCopyItem> &items, bool isWrite, bool localIsDevice) noexcept;
    /* drain every slot once (operator Wait: the window path is already synchronous) */
    Result Drain() noexcept;

private:
    NvmeTransportManager() = default;
    /* release everything on destruction: the singleton dies only at process exit or
     * UT reset, but a live MR/staging pool must never outlive it silently */
    ~NvmeTransportManager() override
    {
        (void)CloseDevice();
    }

    struct IoCtx {
        int32_t done{0};
        int32_t status{0};
    };
    struct QpairSlot {
        void *qpair{nullptr};
        std::mutex mtx;
    };
    struct SlotLease {
        QpairSlot *slot{nullptr};
        std::unique_lock<std::mutex> guard;
    };
    struct NvmeEndpoint {
        void *ctrlr{nullptr};
        void *ns{nullptr};
        std::vector<std::unique_ptr<QpairSlot>> slots;
        std::atomic<uint32_t> nextSlot{0};
        uint32_t maxXferBytes{128U * 1024U};
    };
    using NvmeEndpointPtr = std::shared_ptr<NvmeEndpoint>;

    struct NvmeCmd {
        uint64_t lba{0};
        uint32_t lbaCount{0};
        bool isWrite{false};
        bool rmw{false};
        bool pendingWrite{false};
        bool zerocopy{false};
        char *staging{nullptr};
        char *userPtr{nullptr};
        uint32_t userOff{0};
        uint32_t userLen{0};
        bool localIsDevice{false};
        bool finished{false};
        bool released{false};
        IoCtx ctx{};
    };

    struct ZcRegion {
        uint64_t start{0};
        uint64_t end{0};
    };

    static void IoDone(void *ctx, const void *cpl) noexcept;
    Result GetEndpoint(const HybmNvmeExtent &extent, NvmeEndpointPtr &endpoint) noexcept;
    SlotLease AcquireSlot(const NvmeEndpointPtr &endpoint) noexcept;
    void *AcquireBuf(bool sector) noexcept;
    void ReleaseBuf(void *buf, bool sector) noexcept;
    Result DeviceToLocal(void *dest, const void *src, uint64_t size) noexcept;
    Result LocalToDevice(void *dest, const void *src, uint64_t size) noexcept;
    Result CopyUserOut(const NvmeCmd &cmd) noexcept;
    Result CopyUserIn(const NvmeCmd &cmd) noexcept;
    void BuildCmds(const NvmeEndpointPtr &endpoint, const HybmNvmeExtent &extent, uint64_t offset,
                   char *local, uint64_t len, bool isWrite, bool localIsDevice, bool zerocopy,
                   std::vector<NvmeCmd> &cmds) noexcept;
    bool IsZeroCopyRegistered(const char *local, uint64_t len) const noexcept;
    Result PrepareAndSubmit(const NvmeEndpointPtr &endpoint, QpairSlot *slot, NvmeCmd &cmd) noexcept;
    Result ReapCmd(NvmeCmd &cmd, std::vector<NvmeCmd> *nextRound) noexcept;
    Result SubmitWindowed(const NvmeEndpointPtr &endpoint, QpairSlot *slot, std::vector<NvmeCmd> &cmds,
                          std::vector<NvmeCmd> *nextRound) noexcept;
    void ReleaseCmds(std::vector<NvmeCmd> &cmds) noexcept;

    static std::mutex instanceMutex_;
    static std::shared_ptr<NvmeTransportManager> instance_;

    bool envReady_{false};
    uint64_t chunkSize_{4ULL * 1024 * 1024};
    uint32_t qdepth_{16U};
    uint32_t bufSlots_{16U};
    uint32_t qpairsPerCtrlr_{4U};

    std::mutex endpointsMutex_;
    std::map<std::string, NvmeEndpointPtr> endpoints_;

    mutable std::mutex zcMutex_;
    std::map<uint64_t, ZcRegion> zcRegions_;

    std::mutex bufMutex_;
    std::vector<void *> freeSegBufs_;
    std::vector<void *> freeSectorBufs_;

    std::string nicInfo_;
};

} // namespace transport
} // namespace mf
} // namespace ock

#endif // MF_HYBRID_NVME_TRANSPORT_MANAGER_H
