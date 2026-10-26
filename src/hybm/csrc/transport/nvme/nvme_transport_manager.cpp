/*
 * Copyright: (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You can obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "nvme_transport_manager.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "dl_hybrid_api.h"
#include "dl_nvme_api.h"
#include "hybm_define.h"
#include "hybm_logger.h"
#include "hybm_ptracer.h"

namespace ock {
namespace mf {
namespace transport {

namespace {
constexpr uint64_t NVME_SECTOR_SIZE = 512ULL;

uint64_t EnvToU64(const char *name, uint64_t defVal, uint64_t minVal, uint64_t maxVal)
{
    const char *env = getenv(name);
    if (env == nullptr || env[0] == '\0') {
        return defVal;
    }
    char *end = nullptr;
    unsigned long long val = std::strtoull(env, &end, 10);
    if (end == env) {
        return defVal;
    }
    if (val < minVal) {
        return minVal;
    }
    if (val > maxVal) {
        return maxVal;
    }
    return static_cast<uint64_t>(val);
}

bool localIsDeviceAddr(const void *addr) noexcept
{
    const uint64_t va = reinterpret_cast<uint64_t>(addr);
    return va >= HYBM_DEVICE_VA_START && va < HYBM_DEVICE_VA_START + HYBM_DEVICE_VA_SIZE;
}
} // namespace

std::mutex NvmeTransportManager::instanceMutex_;
std::shared_ptr<NvmeTransportManager> NvmeTransportManager::instance_;

std::shared_ptr<NvmeTransportManager> NvmeTransportManager::GetInstance() noexcept
{
    std::lock_guard<std::mutex> guard(instanceMutex_);
    if (instance_ == nullptr) {
        instance_ = std::shared_ptr<NvmeTransportManager>(new NvmeTransportManager());
        instance_->chunkSize_ = EnvToU64("MF_NVME_BUF_MB", 4ULL, 1ULL, 1024ULL) * 1024ULL * 1024ULL;
        instance_->qdepth_ = static_cast<uint32_t>(EnvToU64("MF_NVME_QDEPTH", 16ULL, 1ULL, 128ULL));
        instance_->qpairsPerCtrlr_ = static_cast<uint32_t>(EnvToU64("MF_NVME_QPAIRS", 4ULL, 1ULL, 16ULL));
        instance_->bufSlots_ = static_cast<uint32_t>(EnvToU64("MF_NVME_BUF_SLOTS", instance_->qdepth_, 1ULL, 128ULL));
        BM_LOG_INFO("nvme transport created, chunk:" << instance_->chunkSize_ << ", qdepth:" << instance_->qdepth_
                                                      << ", bufSlots:" << instance_->bufSlots_
                                                      << ", qpairs:" << instance_->qpairsPerCtrlr_);
    }
    return instance_;
}

void NvmeTransportManager::ResetInstanceForTest() noexcept
{
    std::lock_guard<std::mutex> guard(instanceMutex_);
    instance_ = nullptr;
}

/* ---- TransportManager interface ---- */

Result NvmeTransportManager::OpenDevice(const TransportOptions &options)
{
    (void)options;
    return EnsureEnv();
}

Result NvmeTransportManager::CloseDevice()
{
    /* UT teardown safety: the singleton is destroyed AFTER the API stub restored the
     * dlsym'd pointers to null -- touching them then would crash. With the spdk
     * surface absent there is nothing we can release, just drop the tables. */
    if (DlNvmeApi::gSpdkDmaFree == nullptr || DlNvmeApi::gSpdkNvmeDetach == nullptr) {
        std::lock_guard<std::mutex> zcGuard(zcMutex_);
        zcRegions_.clear();
        std::lock_guard<std::mutex> bufGuard(bufMutex_);
        freeSegBufs_.clear();
        freeSectorBufs_.clear();
        std::lock_guard<std::mutex> epGuard(endpointsMutex_);
        endpoints_.clear();
        return BM_OK;
    }
    /* release leftover zero-copy registrations: the SPDK env outlives the pools
     * (process-wide once-per-success), a live MR would leak ibv contexts */
    {
        std::lock_guard<std::mutex> guard(zcMutex_);
        for (const auto &entry : zcRegions_) {
            const auto ret = DlNvmeApi::MemUnregister(reinterpret_cast<void *>(entry.second.start),
                                                       entry.second.end - entry.second.start);
            if (ret != BM_OK) {
                BM_LOG_WARN("nvme zc unregister at teardown failed, ret:" << ret);
            }
        }
        zcRegions_.clear();
    }
    {
        std::lock_guard<std::mutex> guard(bufMutex_);
        for (auto *buf : freeSegBufs_) {
            DlNvmeApi::DmaFree(buf);
        }
        freeSegBufs_.clear();
        for (auto *buf : freeSectorBufs_) {
            DlNvmeApi::DmaFree(buf);
        }
        freeSectorBufs_.clear();
    }
    {
        std::lock_guard<std::mutex> guard(endpointsMutex_);
        for (auto &entry : endpoints_) {
            for (auto &slot : entry.second->slots) {
                DlNvmeApi::FreeIoQpair(slot->qpair);
            }
            DlNvmeApi::Detach(entry.second->ctrlr);
        }
        endpoints_.clear();
    }
    return BM_OK;
}

Result NvmeTransportManager::RegisterMemoryRegion(const TransportMemoryRegion &mr)
{
    if (mr.addr == 0 || mr.size == 0) {
        return BM_INVALID_PARAM;
    }
    auto *addr = reinterpret_cast<void *>(mr.addr);
    if (localIsDeviceAddr(addr)) {
        return BM_NOT_SUPPORTED; /* device VA: aclrtMemcpy staging semantics, never zc */
    }
    if (!DlNvmeApi::ZeroCopyEnabled()) {
        BM_LOG_DEBUG("nvme zero-copy disabled (MF_NVME_ZEROCOPY=0 or absent symbols)");
        return BM_NOT_SUPPORTED;
    }
    auto ret = EnsureEnv();
    if (ret != BM_OK) {
        return ret;
    }
    /* spdk_mem_register demands a 2MB-aligned mapped range: shrink strictly INWARDS
     * (never touch pages outside the user buffer, neighbours may be unmapped). The
     * head/tail remainders simply keep riding the staging path per command. */
    constexpr uint64_t ALIGN = 2ULL * 1024 * 1024;
    const uint64_t start = reinterpret_cast<uint64_t>(addr);
    const uint64_t end = start + mr.size;
    const uint64_t alignedStart = (start + ALIGN - 1) & ~(ALIGN - 1);
    const uint64_t alignedEnd = end & ~(ALIGN - 1);
    if (alignedEnd <= alignedStart) {
        BM_LOG_WARN("nvme zc register: [0x" << std::hex << start << ", 0x" << end
                    << std::dec << ") has no 2MB-aligned inner window, staging path only");
        return BM_NOT_SUPPORTED;
    }

    TP_TRACE_BEGIN(TP_HYBM_NVME_ZC_REG);
    ret = DlNvmeApi::MemRegister(reinterpret_cast<void *>(alignedStart), alignedEnd - alignedStart);
    TP_TRACE_END(TP_HYBM_NVME_ZC_REG, ret);
    if (ret != BM_OK) {
        BM_LOG_ERROR("nvme zc register [0x" << std::hex << alignedStart << ", 0x" << alignedEnd
                     << std::dec << ") failed, ret:" << ret
                     << " (-EFAULT vtophys/pagemap or memlock, -EINVAL not 2MB-aligned,"
                        " -EBUSY overlapping; IOVA=VA is required for user memory, see InitDpdkEnv)");
        /* spdk_mem_register marks the 2MB pages BEFORE running the notify callbacks and
         * does NOT roll back on failure (v25.05 memory.c) -- a retry on the same range
         * would fail with -EBUSY. Clear the leftover marks now: unregister of a page
         * without an MR is a safe no-op on the rdma map side (translate -> NULL, no dereg). */
        (void)DlNvmeApi::MemUnregister(reinterpret_cast<void *>(alignedStart), alignedEnd - alignedStart);
        return ret;
    }
    std::lock_guard<std::mutex> guard(zcMutex_);
    zcRegions_[start] = ZcRegion{alignedStart, alignedEnd};
    BM_LOG_INFO("nvme zc region registered: user [0x" << std::hex << start << ", 0x" << end
                << ") aligned [0x" << alignedStart << ", 0x" << alignedEnd << std::dec << ") "
                << (alignedEnd - alignedStart) / (1024 * 1024) << "MB zero-copy window");
    return BM_OK;
}

Result NvmeTransportManager::UnregisterMemoryRegion(uint64_t addr)
{
    if (addr == 0) {
        return BM_INVALID_PARAM;
    }
    ZcRegion region{};
    {
        std::lock_guard<std::mutex> guard(zcMutex_);
        auto it = zcRegions_.find(addr);
        if (it == zcRegions_.end()) {
            return BM_INVALID_PARAM; /* not registered (or already unregistered) */
        }
        region = it->second;
        zcRegions_.erase(it);
    }
    auto ret = DlNvmeApi::MemUnregister(reinterpret_cast<void *>(region.start), region.end - region.start);
    if (ret != BM_OK) {
        BM_LOG_ERROR("nvme zc unregister [0x" << std::hex << region.start << std::dec << ") failed, ret:" << ret);
        /* the entry is already dropped: a later hit is impossible, leak the MR at worst */
    }
    return ret;
}

bool NvmeTransportManager::QueryHasRegistered(uint64_t addr, uint64_t size)
{
    return IsZeroCopyRegistered(reinterpret_cast<const char *>(addr), size);
}

Result NvmeTransportManager::QueryMemoryKey(uint64_t addr, TransportMemoryKey &key)
{
    (void)addr;
    (void)key;
    /* NVMe initiator memory is registered for the target to DMA (spdk lkey internal);
     * there is no remote key to hand out -- not applicable */
    return BM_NOT_SUPPORTED;
}

void NvmeTransportManager::UpdateMemoryKey(TransportMemoryKey &key, void *addr)
{
    (void)key;
    (void)addr;
}

Result NvmeTransportManager::Prepare(const HybmTransPrepareOptions &options)
{
    (void)options;
    /* connection topology is driven by extent imports (Warmup), not by rank Prepare */
    return BM_NOT_SUPPORTED;
}

Result NvmeTransportManager::RemoveRanks(const std::vector<uint32_t> &removedRanks)
{
    (void)removedRanks;
    /* ctrlr lifetime follows the extent registry (ns destroy breaks the chain); there is
     * no rank-addressed connection table to shrink */
    return BM_NOT_SUPPORTED;
}

Result NvmeTransportManager::Connect()
{
    return BM_NOT_SUPPORTED; /* see Prepare */
}

Result NvmeTransportManager::AsyncConnect()
{
    return BM_NOT_SUPPORTED; /* see Prepare */
}

Result NvmeTransportManager::WaitForConnected(int64_t timeoutNs)
{
    (void)timeoutNs;
    return BM_NOT_SUPPORTED; /* see Prepare */
}

Result NvmeTransportManager::UpdateRankOptions(const HybmTransPrepareOptions &options)
{
    (void)options;
    return BM_NOT_SUPPORTED; /* see Prepare */
}

const std::string &NvmeTransportManager::GetNic() const
{
    return nicInfo_; /* the NVMe data plane addresses targets by extent, no local NIC binding */
}

const TransportPrivateData NvmeTransportManager::GetPrivateData() const
{
    /* the initiator does not export itself via the exchange chain */
    return TransportPrivateData{};
}

Result NvmeTransportManager::ReadRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    (void)rankId; /* the extent resolved from rAddr carries the owner rank */
    NvmeCopyItem item;
    item.local = reinterpret_cast<char *>(lAddr);
    item.gva = rAddr;
    item.size = size;
    std::vector<NvmeCopyItem> items{item};
    return RunItems(items, false, localIsDeviceAddr(reinterpret_cast<void *>(lAddr)));
}

Result NvmeTransportManager::WriteRemote(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    (void)rankId; /* the extent resolved from rAddr carries the owner rank */
    NvmeCopyItem item;
    item.local = reinterpret_cast<char *>(lAddr);
    item.gva = rAddr;
    item.size = size;
    std::vector<NvmeCopyItem> items{item};
    return RunItems(items, true, localIsDeviceAddr(reinterpret_cast<void *>(lAddr)));
}

Result NvmeTransportManager::ReadRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    /* executed synchronously, the operator's Wait contract stays intact */
    return ReadRemote(rankId, lAddr, rAddr, size);
}

Result NvmeTransportManager::WriteRemoteAsync(uint32_t rankId, uint64_t lAddr, uint64_t rAddr, uint64_t size)
{
    return WriteRemote(rankId, lAddr, rAddr, size);
}

Result NvmeTransportManager::Synchronize(uint32_t rankId)
{
    (void)rankId;
    return Drain();
}

Result NvmeTransportManager::WriteRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor)
{
    (void)rankId;
    (void)descriptor;
    /* the batch entry point of the operator is RunItems (NVMe item shape) */
    return BM_NOT_SUPPORTED;
}

Result NvmeTransportManager::ReadRemoteBatchAsync(uint32_t rankId, const CopyDescriptor &descriptor)
{
    (void)rankId;
    (void)descriptor;
    return BM_NOT_SUPPORTED;
}

/* ---- NVMe domain API ---- */

Result NvmeTransportManager::EnsureEnv() noexcept
{
    if (envReady_) {
        return BM_OK;
    }
    /* process-wide one-shot (dlopen + EAL --in-memory --no-pci + spdk post_init):
     * once-per-success across ALL pools -- post_init is not re-entrant (second
     * same-name DPDK mem event callback registration fails with EEXIST), so a pool
     * destroy / re-create cycle in a live daemon must not drive it again */
    auto ret = DlNvmeApi::EnsureSpdkEnv();
    if (ret != BM_OK) {
        return ret;
    }
    envReady_ = true;
    return BM_OK;
}

Result NvmeTransportManager::Warmup(const HybmNvmeExtent &extent, const std::string &trstring) noexcept
{
    auto ret = EnsureEnv();
    if (ret != BM_OK) {
        return ret;
    }

    std::string trsvcid = std::to_string(extent.trsvcid);
    std::string key = std::string(extent.nqn) + "|" + extent.traddr + "|" + trsvcid + "|" +
                      std::to_string(extent.nsid);
    {
        std::lock_guard<std::mutex> guard(endpointsMutex_);
        if (endpoints_.find(key) != endpoints_.end()) {
            return BM_OK; /* already warmed (idempotent), shared across pools */
        }
    }

    auto candidate = std::make_shared<NvmeEndpoint>();
    MfSpdkNvmeTransportId trid{};
    (void)strncpy(trid.trstring, trstring.c_str(), sizeof(trid.trstring) - 1);
    trid.trtype = (trstring == "tcp") ? MF_SPDK_NVME_TRTYPE_TCP : MF_SPDK_NVME_TRTYPE_RDMA;
    (void)strncpy(trid.traddr, extent.traddr, sizeof(trid.traddr) - 1);
    (void)strncpy(trid.trsvcid, trsvcid.c_str(), sizeof(trid.trsvcid) - 1);
    (void)strncpy(trid.subnqn, extent.nqn, sizeof(trid.subnqn) - 1);
    trid.adrfam = MF_SPDK_NVMF_ADRFAM_IPV4;

    ret = DlNvmeApi::Connect(trid, candidate->ctrlr);
    if (ret != BM_OK) {
        BM_LOG_ERROR("nvme warmup connect to " << extent.traddr << ":" << extent.trsvcid
                                               << " nqn: " << extent.nqn << " nsid: " << extent.nsid
                                               << " failed, ret:" << ret);
        return BM_NOT_CONNECTED;
    }
    ret = DlNvmeApi::GetNs(candidate->ctrlr, extent.nsid, candidate->ns);
    if (ret != BM_OK) {
        BM_LOG_ERROR("nvme warmup get ns " << extent.nsid << " failed, ret:" << ret);
        DlNvmeApi::Detach(candidate->ctrlr);
        return ret;
    }
    const uint32_t xfer = DlNvmeApi::MaxXferSize(candidate->ctrlr);
    if (xfer >= NVME_SECTOR_SIZE) {
        candidate->maxXferBytes = (xfer / NVME_SECTOR_SIZE) * NVME_SECTOR_SIZE;
    }
    for (uint32_t i = 0; i < qpairsPerCtrlr_; ++i) {
        auto slot = std::make_unique<QpairSlot>();
        Result qret = DlNvmeApi::AllocIoQpair(candidate->ctrlr, slot->qpair);
        if (qret != BM_OK) {
            BM_LOG_ERROR("nvme warmup alloc io qpair failed, ret:" << qret);
            for (auto &s : candidate->slots) {
                DlNvmeApi::FreeIoQpair(s->qpair);
            }
            DlNvmeApi::Detach(candidate->ctrlr);
            return qret;
        }
        candidate->slots.push_back(std::move(slot));
    }

    {
        std::lock_guard<std::mutex> guard(endpointsMutex_);
        /* another thread may have won the race, keep the existing one and drop ours */
        auto it = endpoints_.find(key);
        if (it != endpoints_.end()) {
            for (auto &s : candidate->slots) {
                DlNvmeApi::FreeIoQpair(s->qpair);
            }
            DlNvmeApi::Detach(candidate->ctrlr);
            return BM_OK;
        }
        endpoints_[key] = candidate;
    }
    BM_LOG_INFO("nvme endpoint warmed up, nsid:" << extent.nsid << ", qpairs:" << candidate->slots.size()
                                                 << ", maxXfer:" << candidate->maxXferBytes);
    return BM_OK;
}

void NvmeTransportManager::IoDone(void *ctx, const void *cpl) noexcept
{
    if (ctx == nullptr) {
        return;
    }
    auto *io = static_cast<IoCtx *>(ctx);
    /* self-declared prefix of v25.05 struct spdk_nvme_cpl: the first 16 bits are the
     * status word (bit0 phase, bits1-8 SC, bits9-11 SCT); spdk_nvme_cpl_is_success is
     * exactly (SC == 0 && SCT == 0), i.e. (raw & 0x0FFE) == 0 */
    uint32_t status = 0;
    if (cpl != nullptr) {
        uint16_t raw = 0;
        (void)memcpy(&raw, cpl, sizeof(raw));
        if ((raw & 0x0FFE) != 0) {
            status = 1;
        }
    }
    io->status = static_cast<int32_t>(status);
    io->done = 1;
}

Result NvmeTransportManager::GetEndpoint(const HybmNvmeExtent &extent, NvmeEndpointPtr &endpoint) noexcept
{
    std::string trsvcid = std::to_string(extent.trsvcid);
    std::string key = std::string(extent.nqn) + "|" + extent.traddr + "|" + trsvcid + "|" +
                      std::to_string(extent.nsid);
    std::lock_guard<std::mutex> guard(endpointsMutex_);
    auto it = endpoints_.find(key);
    if (it == endpoints_.end()) {
        /* by design endpoints only appear via Warmup (extent import, hard-fail); reaching
         * here means the import-time warmup was skipped or lost -- refuse to connect
         * lazily so the broken setup surfaces instead of being papered over */
        BM_LOG_ERROR("nvme endpoint not warmed up (import-time warmup missing), key:" << key);
        return BM_NOT_CONNECTED;
    }
    endpoint = it->second;
    return BM_OK;
}

NvmeTransportManager::SlotLease NvmeTransportManager::AcquireSlot(const NvmeEndpointPtr &endpoint) noexcept
{
    TP_TRACE_BEGIN(TP_HYBM_NVME_QPAIR_WAIT);
    const uint32_t n = static_cast<uint32_t>(endpoint->slots.size());
    const uint32_t start = endpoint->nextSlot.fetch_add(1, std::memory_order_relaxed) % n;
    for (uint32_t i = 0; i < n; ++i) {
        auto &slot = endpoint->slots[(start + i) % n];
        std::unique_lock<std::mutex> guard(slot->mtx, std::try_to_lock);
        if (guard.owns_lock()) {
            TP_TRACE_END(TP_HYBM_NVME_QPAIR_WAIT, 0);
            return SlotLease{slot.get(), std::move(guard)};
        }
    }
    /* all slots busy: block on the rotation slot -- natural backpressure, the window
     * concurrency ceiling is the slot count (MF_NVME_QPAIRS) */
    auto &slot = endpoint->slots[start];
    std::unique_lock<std::mutex> guard(slot->mtx);
    TP_TRACE_END(TP_HYBM_NVME_QPAIR_WAIT, 0);
    return SlotLease{slot.get(), std::move(guard)};
}

void *NvmeTransportManager::AcquireBuf(bool sector) noexcept
{
    {
        std::lock_guard<std::mutex> guard(bufMutex_);
        auto &pool = sector ? freeSectorBufs_ : freeSegBufs_;
        if (!pool.empty()) {
            void *buf = pool.back();
            pool.pop_back();
            return buf;
        }
    }
    void *buf = nullptr;
    const size_t size = sector ? NVME_SECTOR_SIZE : chunkSize_;
    if (DlNvmeApi::DmaMalloc(size, NVME_SECTOR_SIZE, buf) != BM_OK) {
        return nullptr;
    }
    return buf;
}

void NvmeTransportManager::ReleaseBuf(void *buf, bool sector) noexcept
{
    if (buf == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(bufMutex_);
    auto &pool = sector ? freeSectorBufs_ : freeSegBufs_;
    if (pool.size() < bufSlots_) {
        pool.push_back(buf);
        return;
    }
    DlNvmeApi::DmaFree(buf);
}

Result NvmeTransportManager::DeviceToLocal(void *dest, const void *src, uint64_t size) noexcept
{
    return DlHybridApi::Memcpy(dest, size, src, size, ACL_MEMCPY_DEVICE_TO_HOST);
}

Result NvmeTransportManager::LocalToDevice(void *dest, const void *src, uint64_t size) noexcept
{
    return DlHybridApi::Memcpy(dest, size, src, size, ACL_MEMCPY_HOST_TO_DEVICE);
}

Result NvmeTransportManager::CopyUserOut(const NvmeCmd &cmd) noexcept
{
    if (cmd.localIsDevice) {
        return LocalToDevice(cmd.userPtr, cmd.staging + cmd.userOff, cmd.userLen);
    }
    (void)memcpy(cmd.userPtr, cmd.staging + cmd.userOff, cmd.userLen);
    return BM_OK;
}

Result NvmeTransportManager::CopyUserIn(const NvmeCmd &cmd) noexcept
{
    if (cmd.localIsDevice) {
        return DeviceToLocal(cmd.staging + cmd.userOff, cmd.userPtr, cmd.userLen);
    }
    (void)memcpy(cmd.staging + cmd.userOff, cmd.userPtr, cmd.userLen);
    return BM_OK;
}

bool NvmeTransportManager::IsZeroCopyRegistered(const char *local, uint64_t len) const noexcept
{
    if (local == nullptr || len == 0) {
        return false;
    }
    const uint64_t start = reinterpret_cast<uint64_t>(local);
    std::lock_guard<std::mutex> guard(zcMutex_);
    if (zcRegions_.empty()) {
        return false;
    }
    auto it = zcRegions_.upper_bound(start);
    if (it != zcRegions_.begin()) {
        --it;
        return start >= it->second.start && start + len <= it->second.end;
    }
    return false;
}

void NvmeTransportManager::BuildCmds(const NvmeEndpointPtr &endpoint, const HybmNvmeExtent &extent, uint64_t offset,
                                     char *local, uint64_t len, bool isWrite, bool localIsDevice, bool zerocopy,
                                     std::vector<NvmeCmd> &cmds) noexcept
{
    const uint64_t absStart = offset;
    const uint64_t absEnd = offset + len;
    const uint64_t firstSector = absStart >> 9;
    const uint64_t lastSector = (absEnd - 1) >> 9;
    const uint32_t headSkip = static_cast<uint32_t>(absStart & (NVME_SECTOR_SIZE - 1));
    /* head clamps to len (a sub-sector write inside ONE sector must not stretch past the
     * user buffer) and tail only exists when the range actually leaves the first sector */
    const uint32_t headBytes =
        headSkip != 0 ? static_cast<uint32_t>(std::min<uint64_t>(NVME_SECTOR_SIZE - headSkip, len)) : 0U;
    const uint32_t tailBytes = (lastSector != firstSector) ? static_cast<uint32_t>(absEnd & (NVME_SECTOR_SIZE - 1)) : 0U;
    const uint64_t midStart = absStart + headBytes;
    const uint64_t midLen = len - headBytes - tailBytes;

    /* sub-sector head: one sector RMW (write path: read-half queued into the shared
     * window, merged and re-submitted in round 2; read path: full sector read, user
     * bytes sliced out on reap) */
    if (headBytes != 0) {
        NvmeCmd cmd{};
        cmd.lba = extent.lbaBase + firstSector;
        cmd.lbaCount = 1;
        cmd.rmw = true;
        cmd.isWrite = false;
        cmd.pendingWrite = isWrite;
        cmd.userPtr = local;
        cmd.userOff = headSkip;
        cmd.userLen = headBytes;
        cmd.localIsDevice = localIsDevice;
        cmds.push_back(cmd);
    }

    /* mid: full-sector direct IO, split by min(ctrlr MDTS, staging chunk); spdk does not
     * split oversized IO itself, a target (e.g. nvmf TCP, 128K default) would reject it.
     * zero-copy is decided PER COMMAND: a 4K-aligned-but-not-2MB user buffer yields mid
     * segments fully inside the registered window (hit) and edge segments (staging) */
    if (midLen != 0) {
        const uint64_t segCap = std::min<uint64_t>(std::min<uint64_t>(endpoint->maxXferBytes, chunkSize_),
                                                   midLen) &
                                 ~(NVME_SECTOR_SIZE - 1);
        uint64_t submitted = 0;
        while (submitted < midLen) {
            const uint64_t n = std::min<uint64_t>(segCap, midLen - submitted);
            NvmeCmd cmd{};
            cmd.lba = extent.lbaBase + ((midStart + submitted) >> 9);
            cmd.lbaCount = static_cast<uint32_t>(n >> 9);
            cmd.isWrite = isWrite;
            cmd.zerocopy = zerocopy && IsZeroCopyRegistered(local + headBytes + submitted, n);
            cmd.userPtr = local + headBytes + submitted;
            cmd.userOff = 0;
            cmd.userLen = static_cast<uint32_t>(n);
            cmd.localIsDevice = localIsDevice;
            cmds.push_back(cmd);
            submitted += n;
        }
    }

    if (tailBytes != 0) {
        NvmeCmd cmd{};
        cmd.lba = extent.lbaBase + lastSector;
        cmd.lbaCount = 1;
        cmd.rmw = true;
        cmd.isWrite = false;
        cmd.pendingWrite = isWrite;
        cmd.userPtr = local + (len - tailBytes);
        cmd.userOff = 0;
        cmd.userLen = tailBytes;
        cmd.localIsDevice = localIsDevice;
        cmds.push_back(cmd);
    }
}

Result NvmeTransportManager::PrepareAndSubmit(const NvmeEndpointPtr &endpoint, QpairSlot *slot, NvmeCmd &cmd) noexcept
{
    /* zero-copy command: the NIC DMAs the user buffer itself, no staging, no copies */
    char *dmaBuf = nullptr;
    if (cmd.zerocopy) {
        dmaBuf = cmd.userPtr + cmd.userOff;
    } else if (cmd.staging == nullptr) {
        /* staging is taken lazily here (NOT in BuildCmds): only qdepth commands are in
         * flight at any time, so the pools only need qdepth blocks */
        cmd.staging = static_cast<char *>(AcquireBuf(cmd.rmw));
        if (cmd.staging == nullptr) {
            BM_LOG_ERROR("nvme staging buffer exhausted");
            return BM_MALLOC_FAILED;
        }
        /* mid writes stage the user bytes in before submit; reads and RMW read-halves
         * are filled by the device */
        if (cmd.isWrite && !cmd.pendingWrite) {
            const uint64_t t0 = TP_CURRENT_TIME_NS;
            auto ret = CopyUserIn(cmd);
            TP_TRACE_RECORD(TP_HYBM_NVME_STAGE_COPY, TP_CURRENT_TIME_NS - t0, ret);
            if (ret != BM_OK) {
                return ret;
            }
        }
        dmaBuf = cmd.staging;
    } else {
        /* else: round-2 RMW command -- the staging (sector already read + merged by ReapCmd)
         * rides along, no re-acquire (would leak the transferred block) and no re-merge */
        dmaBuf = cmd.staging;
    }
    const int rc = cmd.isWrite
        ? DlNvmeApi::NsCmdWrite(endpoint->ns, slot->qpair, dmaBuf, cmd.lba, cmd.lbaCount, IoDone, &cmd.ctx)
        : DlNvmeApi::NsCmdRead(endpoint->ns, slot->qpair, dmaBuf, cmd.lba, cmd.lbaCount, IoDone, &cmd.ctx);
    TP_TRACE_RECORD(TP_HYBM_NVME_SUBMIT, 0, rc == 0 ? 0 : 1);
    if (rc != 0) {
        BM_LOG_ERROR("nvme submit " << (cmd.isWrite ? "write" : "read") << " failed, rc:" << rc
                                    << ", lba:" << cmd.lba << ", lbaCount:" << cmd.lbaCount);
        return BM_ERROR;
    }
    return BM_OK;
}

Result NvmeTransportManager::ReapCmd(NvmeCmd &cmd, std::vector<NvmeCmd> *nextRound) noexcept
{
    if (cmd.pendingWrite) {
        /* write-path RMW read-half done: merge the user bytes into the sector and hand
         * the very same staging buffer to the round-2 write command */
        const uint64_t t0 = TP_CURRENT_TIME_NS;
        auto ret = CopyUserIn(cmd);
        TP_TRACE_RECORD(TP_HYBM_NVME_STAGE_COPY, TP_CURRENT_TIME_NS - t0, ret);
        if (ret != BM_OK) {
            ReleaseBuf(cmd.staging, cmd.rmw);
            cmd.released = true;
            return ret;
        }
        NvmeCmd wr = cmd;
        wr.isWrite = true;
        wr.pendingWrite = false;
        wr.finished = false;
        wr.released = false;
        wr.ctx = IoCtx{};
        cmd.released = true; /* staging ownership moved to the round-2 command */
        nextRound->push_back(wr);
        return BM_OK;
    }
    if (!cmd.isWrite) {
        /* read: zero-copy commands landed straight in the user buffer, nothing to drain */
        if (cmd.zerocopy) {
            cmd.released = true; /* no staging was ever taken */
            return BM_OK;
        }
        /* slice the user bytes out of staging into the local side */
        const uint64_t t0 = TP_CURRENT_TIME_NS;
        auto ret = CopyUserOut(cmd);
        TP_TRACE_RECORD(TP_HYBM_NVME_STAGE_COPY, TP_CURRENT_TIME_NS - t0, ret);
        ReleaseBuf(cmd.staging, cmd.rmw);
        cmd.released = true;
        return ret;
    }
    /* mid write: the CQE is the acknowledgment, staging is free */
    if (!cmd.zerocopy) {
        ReleaseBuf(cmd.staging, cmd.rmw);
    }
    cmd.released = true;
    return BM_OK;
}

Result NvmeTransportManager::SubmitWindowed(const NvmeEndpointPtr &endpoint, QpairSlot *slot,
                                            std::vector<NvmeCmd> &cmds, std::vector<NvmeCmd> *nextRound) noexcept
{
    size_t next = 0;  /* first not-yet-submitted command */
    size_t base = 0;  /* first not-yet-reaped command */
    uint32_t inflight = 0;
    Result firstErr = BM_OK;

    while (base < cmds.size() || inflight > 0) {
        /* fill the window up to qdepth */
        while (firstErr == BM_OK && inflight < qdepth_ && next < cmds.size()) {
            auto ret = PrepareAndSubmit(endpoint, slot, cmds[next]);
            if (ret != BM_OK) {
                firstErr = ret; /* stop issuing, keep draining what is in flight */
                break;
            }
            ++next;
            ++inflight;
        }
        if (inflight == 0) {
            break; /* everything submitted got reaped (or nothing could be submitted) */
        }

        TP_TRACE_BEGIN(TP_HYBM_NVME_POLL_WAIT);
        const int32_t reaped = DlNvmeApi::ProcessCompletions(slot->qpair);
        TP_TRACE_END(TP_HYBM_NVME_POLL_WAIT, reaped < 0 ? 1 : 0);
        if (reaped < 0) {
            BM_LOG_ERROR("nvme qpair process completions failed");
            firstErr = firstErr == BM_OK ? BM_ERROR : firstErr;
            break; /* the qpair is dead: completions will never arrive, abandon them */
        }

        for (size_t i = base; i < next; ++i) {
            NvmeCmd &cmd = cmds[i];
            if (cmd.finished || cmd.ctx.done == 0) {
                continue;
            }
            cmd.finished = true;
            --inflight;
            if (cmd.ctx.status != 0 && firstErr == BM_OK) {
                BM_LOG_ERROR("nvme command failed in cpl, lba:" << cmd.lba << ", lbaCount:" << cmd.lbaCount);
                firstErr = BM_ERROR;
            }
            if (firstErr == BM_OK) {
                auto ret = ReapCmd(cmd, nextRound);
                if (ret != BM_OK) {
                    firstErr = ret;
                }
            } else if (!cmd.released) {
                /* error already recorded: just drop the staging, no side effects */
                ReleaseBuf(cmd.staging, cmd.rmw);
                cmd.released = true;
            }
        }
        while (base < next && cmds[base].finished) {
            ++base;
        }
    }
    return firstErr;
}

void NvmeTransportManager::ReleaseCmds(std::vector<NvmeCmd> &cmds) noexcept
{
    for (auto &cmd : cmds) {
        if (!cmd.released && cmd.staging != nullptr) {
            ReleaseBuf(cmd.staging, cmd.rmw);
            cmd.released = true;
        }
    }
}

Result NvmeTransportManager::RunItems(std::vector<NvmeCopyItem> &items, bool isWrite, bool localIsDevice) noexcept
{
    /* resolve every item to (extent, endpoint) and group per endpoint: a window is bound
     * to one qpair slot, commands of different endpoints cannot share it */
    struct Group {
        NvmeEndpointPtr endpoint;
        HybmNvmeExtent extent;
        std::vector<size_t> itemIdx;
    };
    std::vector<Group> groups;
    std::map<std::string, size_t> groupByKey;
    for (size_t i = 0; i < items.size(); ++i) {
        TP_TRACE_BEGIN(TP_HYBM_NVME_LOOKUP);
        HybmNvmeExtent extent;
        const bool found = HybmNvmeExtentRegistry::GetInstance().Lookup(items[i].gva, items[i].size, extent);
        TP_TRACE_END(TP_HYBM_NVME_LOOKUP, found ? 0 : 1);
        if (!found) {
            BM_LOG_ERROR("gva 0x" << std::hex << items[i].gva << std::dec << " size " << items[i].size
                                  << " is not a registered nvme extent");
            return BM_INVALID_PARAM;
        }
        NvmeEndpointPtr endpoint;
        auto ret = GetEndpoint(extent, endpoint);
        if (ret != BM_OK) {
            return ret;
        }
        const std::string key = std::string(extent.nqn) + "|" + extent.traddr + "|" +
                                std::to_string(extent.trsvcid) + "|" + std::to_string(extent.nsid);
        auto it = groupByKey.find(key);
        if (it == groupByKey.end()) {
            groupByKey[key] = groups.size();
            Group group;
            group.endpoint = endpoint;
            group.extent = extent;
            groups.push_back(std::move(group));
            it = groupByKey.find(key);
        }
        groups[it->second].itemIdx.push_back(i);
    }

    /* batch semantics follow host_rdma: a failing group is recorded and the remaining
     * groups still run; the first error is returned after everyone got its chance */
    Result firstErr = BM_OK;
    for (auto &group : groups) {
        std::vector<NvmeCmd> cmds;
        cmds.reserve(group.itemIdx.size() * 2U + group.itemIdx.size());
        for (size_t idx : group.itemIdx) {
            const auto &item = items[idx];
            /* zero-copy arming only excludes device endpoints; the PER-COMMAND decision
             * happens inside BuildCmds (each mid segment must FULLY ride a registered
             * window). Arming on a whole-item hit would misclassify a 4K-aligned (not
             * 2MB) buffer: the item as a whole never fits the window, yet most of its
             * mid segments do -- they must still DMA directly. An empty MR table makes
             * every per-command lookup return early, so arming is zero-cost then. */
            const bool zc = !localIsDevice;
            BuildCmds(group.endpoint, group.extent, item.gva - group.extent.gvaBase, item.local, item.size,
                      isWrite, localIsDevice, zc, cmds);
        }

        auto lease = AcquireSlot(group.endpoint);
        std::vector<NvmeCmd> stageB;
        auto ret = SubmitWindowed(group.endpoint, lease.slot, cmds, &stageB);
        if (ret == BM_OK && !stageB.empty()) {
            TP_TRACE_BEGIN(TP_HYBM_NVME_RMW_STAGE_B);
            ret = SubmitWindowed(group.endpoint, lease.slot, stageB, nullptr);
            TP_TRACE_END(TP_HYBM_NVME_RMW_STAGE_B, ret);
        }
        ReleaseCmds(cmds);
        ReleaseCmds(stageB);
        if (ret != BM_OK) {
            BM_LOG_ERROR("nvme window failed for " << group.itemIdx.size() << " item(s), ret:" << ret);
            if (firstErr == BM_OK) {
                firstErr = ret;
            }
        }
    }
    return firstErr;
}

Result NvmeTransportManager::Drain() noexcept
{
    /* the window path polls its own qpairs to completion; drain stragglers defensively */
    std::vector<NvmeEndpointPtr> snapshot;
    {
        std::lock_guard<std::mutex> guard(endpointsMutex_);
        for (auto &entry : endpoints_) {
            snapshot.push_back(entry.second);
        }
    }
    for (auto &endpoint : snapshot) {
        for (auto &slot : endpoint->slots) {
            (void)DlNvmeApi::ProcessCompletions(slot->qpair);
        }
    }
    return BM_OK;
}

} // namespace transport
} // namespace mf
} // namespace ock
