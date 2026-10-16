/*
 * Copyright: (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include "hybm_data_op_nvme.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

#include "dl_hybrid_api.h"

namespace ock {
namespace mf {

namespace {
constexpr uint64_t NVME_SECTOR_SIZE = 512ULL;

struct IoCtx {
    std::atomic<int32_t> done{0};
};

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
} // namespace

HostDataOpNvme::HostDataOpNvme(uint32_t rankId) noexcept : rankId_(rankId)
{
    chunkSize_ = EnvToU64("MF_NVME_BUF_MB", 4ULL, 1ULL, 1024ULL) * 1024ULL * 1024ULL;
    bufSlots_ = static_cast<uint32_t>(EnvToU64("MF_NVME_BUF_SLOTS", 4ULL, 1ULL, 64ULL));
    qpairsPerCtrlr_ = static_cast<uint32_t>(EnvToU64("MF_NVME_QPAIRS", 2ULL, 1ULL, 16ULL));
}

HostDataOpNvme::~HostDataOpNvme()
{
    UnInitialize();
}

Result HostDataOpNvme::Initialize() noexcept
{
    if (inited_) {
        return BM_OK;
    }
    inited_ = true;
    BM_LOG_INFO("nvme data operator initialized, rank:" << rankId_ << ", chunk:" << chunkSize_
                                                        << ", bufSlots:" << bufSlots_
                                                        << ", qpairs:" << qpairsPerCtrlr_);
    return BM_OK;
}

void HostDataOpNvme::UnInitialize() noexcept
{
    std::lock_guard<std::mutex> ioGuard(ioMutex_);
    inited_ = false;
    ready_ = false;

    if (scratch_ != nullptr) {
        DlNvmeApi::DmaFree(scratch_);
        scratch_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> guard(bufMutex_);
        for (auto *buf : freeBufs_) {
            DlNvmeApi::DmaFree(buf);
        }
        freeBufs_.clear();
    }
    {
        std::lock_guard<std::mutex> guard(endpointsMutex_);
        for (auto &entry : endpoints_) {
            for (auto *qpair : entry.second->qpairs) {
                DlNvmeApi::FreeIoQpair(qpair);
            }
            DlNvmeApi::Detach(entry.second->ctrlr);
        }
        endpoints_.clear();
    }
}

void HostDataOpNvme::CleanUp() noexcept
{
    UnInitialize();
}

bool HostDataOpNvme::IsSupportedDirection(hybm_data_copy_direction direction) const noexcept
{
    switch (direction) {
        case HYBM_LOCAL_HOST_TO_GLOBAL_NVME:
        case HYBM_GLOBAL_NVME_TO_LOCAL_HOST:
        case HYBM_LOCAL_DEVICE_TO_GLOBAL_NVME:
        case HYBM_GLOBAL_NVME_TO_LOCAL_DEVICE:
            return true;
        default:
            return false;
    }
}

void HostDataOpNvme::IoDone(void *ctx, const void *cpl) noexcept
{
    (void)cpl;
    if (ctx != nullptr) {
        static_cast<IoCtx *>(ctx)->done.store(1, std::memory_order_release);
    }
}

Result HostDataOpNvme::EnsureReady() noexcept
{
    if (ready_) {
        return BM_OK;
    }

    auto ret = DlNvmeApi::LoadLibrary();
    if (ret != BM_OK) {
        BM_LOG_ERROR("nvme data operator load spdk failed, ret:" << ret);
        return ret;
    }

    /* switch the (externally pre-inited or fresh) SPDK env to dynamic memory
     * registration mode; failure is tolerated, connect will surface real problems */
    if (DlNvmeApi::EnvDpdkPostInit(true) != 0) {
        BM_LOG_WARN("spdk_env_dpdk_post_init failed or absent, nvme io may fail on unregistered buffers");
    }

    void *scratch = nullptr;
    ret = DlNvmeApi::DmaMalloc(NVME_SECTOR_SIZE, NVME_SECTOR_SIZE, scratch);
    if (ret != BM_OK) {
        BM_LOG_ERROR("nvme data operator alloc rmw scratch failed");
        return ret;
    }
    scratch_ = static_cast<char *>(scratch);
    ready_ = true;
    return BM_OK;
}

Result HostDataOpNvme::GetEndpoint(const HybmNvmeExtent &extent, NvmeEndpointPtr &endpoint) noexcept
{
    std::string trsvcid = std::to_string(extent.trsvcid);
    std::string key = std::string(extent.nqn) + "|" + extent.traddr + "|" + trsvcid + "|" +
                      std::to_string(extent.nsid);
    {
        std::lock_guard<std::mutex> guard(endpointsMutex_);
        auto it = endpoints_.find(key);
        if (it != endpoints_.end()) {
            endpoint = it->second;
            return BM_OK;
        }
    }

    auto candidate = std::make_shared<NvmeEndpoint>();
    MfSpdkNvmeTransportId trid{};
    const char *trstr = "rdma";
    (void)strncpy(trid.trstring, trstr, sizeof(trid.trstring) - 1);
    (void)strncpy(trid.traddr, extent.traddr, sizeof(trid.traddr) - 1);
    (void)strncpy(trid.trsvcid, trsvcid.c_str(), sizeof(trid.trsvcid) - 1);
    (void)strncpy(trid.subnqn, extent.nqn, sizeof(trid.subnqn) - 1);
    trid.adrfam = 0; /* IPv4 */

    Result ret = DlNvmeApi::Connect(trid, candidate->ctrlr);
    if (ret != BM_OK) {
        BM_LOG_ERROR("nvme connect to " << extent.traddr << ":" << extent.trsvcid << " failed, ret:" << ret);
        return BM_NOT_CONNECTED;
    }
    ret = DlNvmeApi::GetNs(candidate->ctrlr, extent.nsid, candidate->ns);
    if (ret != BM_OK) {
        BM_LOG_ERROR("nvme get ns " << extent.nsid << " failed, ret:" << ret);
        DlNvmeApi::Detach(candidate->ctrlr);
        return ret;
    }
    for (uint32_t i = 0; i < qpairsPerCtrlr_; ++i) {
        void *qpair = nullptr;
        ret = DlNvmeApi::AllocIoQpair(candidate->ctrlr, qpair);
        if (ret != BM_OK) {
            BM_LOG_ERROR("nvme alloc io qpair failed, ret:" << ret);
            for (auto *q : candidate->qpairs) {
                DlNvmeApi::FreeIoQpair(q);
            }
            DlNvmeApi::Detach(candidate->ctrlr);
            return ret;
        }
        candidate->qpairs.push_back(qpair);
    }

    std::lock_guard<std::mutex> guard(endpointsMutex_);
    /* another thread may have won the race, keep the existing one and drop ours */
    auto it = endpoints_.find(key);
    if (it != endpoints_.end()) {
        endpoint = it->second;
        for (auto *qpair : candidate->qpairs) {
            DlNvmeApi::FreeIoQpair(qpair);
        }
        DlNvmeApi::Detach(candidate->ctrlr);
        return BM_OK;
    }
    endpoints_[key] = candidate;
    endpoint = candidate;
    BM_LOG_INFO("nvme endpoint connected, nsid:" << extent.nsid << ", qpairs:" << candidate->qpairs.size());
    return BM_OK;
}

void *HostDataOpNvme::NextQpair(const NvmeEndpointPtr &endpoint) noexcept
{
    uint32_t idx = endpoint->nextQpair % endpoint->qpairs.size();
    endpoint->nextQpair += 1U;
    return endpoint->qpairs[idx];
}

Result HostDataOpNvme::SubmitAndWait(const NvmeEndpointPtr &endpoint, void *qpair, bool isWrite, uint64_t lba,
                                     uint32_t lbaCount, void *buf) noexcept
{
    IoCtx ctx;
    int rc;
    if (isWrite) {
        rc = DlNvmeApi::NsCmdWrite(endpoint->ns, qpair, buf, lba, lbaCount, IoDone, &ctx);
    } else {
        rc = DlNvmeApi::NsCmdRead(endpoint->ns, qpair, buf, lba, lbaCount, IoDone, &ctx);
    }
    if (rc != 0) {
        BM_LOG_ERROR("nvme submit " << (isWrite ? "write" : "read") << " failed, rc:" << rc << ", lba:" << lba
                                    << ", lbaCount:" << lbaCount);
        return BM_ERROR;
    }
    /* on-demand polling: the qpair is only driven while one of our commands is in flight */
    while (ctx.done.load(std::memory_order_acquire) == 0) {
        if (DlNvmeApi::ProcessCompletions(qpair) < 0) {
            BM_LOG_ERROR("nvme qpair process completions failed");
            return BM_ERROR;
        }
    }
    return BM_OK;
}

Result HostDataOpNvme::RmwSector(const NvmeEndpointPtr &endpoint, uint64_t sectorLba, uint32_t offInSector,
                                 uint32_t n, char *ioBytes, bool isWrite) noexcept
{
    if (isWrite) {
        auto ret = SubmitAndWait(endpoint, NextQpair(endpoint), false, sectorLba, 1, scratch_);
        if (ret != BM_OK) {
            return ret;
        }
        (void)memcpy(scratch_ + offInSector, ioBytes, n);
        return SubmitAndWait(endpoint, NextQpair(endpoint), true, sectorLba, 1, scratch_);
    }
    auto ret = SubmitAndWait(endpoint, NextQpair(endpoint), false, sectorLba, 1, scratch_);
    if (ret != BM_OK) {
        return ret;
    }
    (void)memcpy(ioBytes, scratch_ + offInSector, n);
    return BM_OK;
}

Result HostDataOpNvme::SubmitRange(const NvmeEndpointPtr &endpoint, const HybmNvmeExtent &extent, uint64_t offset,
                                   const void *buf, uint64_t len, bool isWrite) noexcept
{
    const auto *bytes = static_cast<const char *>(buf);
    const uint64_t absStart = offset;
    const uint64_t absEnd = offset + len;
    const uint64_t firstSector = absStart >> 9;
    const uint64_t lastSector = (absEnd - 1) >> 9;

    /* single sector: full-sector aligned goes straight through, sub-sector is RMW */
    if (firstSector == lastSector) {
        const uint32_t offInSector = static_cast<uint32_t>(absStart & (NVME_SECTOR_SIZE - 1));
        if (offInSector == 0 && len == NVME_SECTOR_SIZE) {
            return SubmitAndWait(endpoint, NextQpair(endpoint), isWrite, extent.lbaBase + firstSector, 1,
                                 const_cast<char *>(bytes));
        }
        return RmwSector(endpoint, extent.lbaBase + firstSector, offInSector, static_cast<uint32_t>(len),
                         const_cast<char *>(bytes), isWrite);
    }

    const uint32_t headSkip = static_cast<uint32_t>(absStart & (NVME_SECTOR_SIZE - 1));
    const uint32_t headBytes = headSkip != 0 ? static_cast<uint32_t>(NVME_SECTOR_SIZE - headSkip) : 0U;
    const uint32_t tailBytes = static_cast<uint32_t>(absEnd & (NVME_SECTOR_SIZE - 1));
    const uint64_t midStart = absStart + headBytes;
    const uint64_t midLen = len - headBytes - tailBytes;

    if (headBytes != 0) {
        auto ret = RmwSector(endpoint, extent.lbaBase + firstSector, headSkip, headBytes,
                             const_cast<char *>(bytes), isWrite);
        if (ret != BM_OK) {
            return ret;
        }
    }

    if (midLen != 0) {
        auto ret = SubmitAndWait(endpoint, NextQpair(endpoint), isWrite, extent.lbaBase + (midStart >> 9),
                                 static_cast<uint32_t>(midLen >> 9), const_cast<char *>(bytes + headBytes));
        if (ret != BM_OK) {
            return ret;
        }
    }

    if (tailBytes != 0) {
        auto ret = RmwSector(endpoint, extent.lbaBase + lastSector, 0, tailBytes,
                             const_cast<char *>(bytes + (len - tailBytes)), isWrite);
        if (ret != BM_OK) {
            return ret;
        }
    }
    return BM_OK;
}

void *HostDataOpNvme::AcquireBuf() noexcept
{
    {
        std::lock_guard<std::mutex> guard(bufMutex_);
        if (!freeBufs_.empty()) {
            void *buf = freeBufs_.back();
            freeBufs_.pop_back();
            return buf;
        }
    }
    void *buf = nullptr;
    if (DlNvmeApi::DmaMalloc(chunkSize_, NVME_SECTOR_SIZE, buf) != BM_OK) {
        return nullptr;
    }
    return buf;
}

void HostDataOpNvme::ReleaseBuf(void *buf) noexcept
{
    if (buf == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(bufMutex_);
    if (freeBufs_.size() < bufSlots_) {
        freeBufs_.push_back(buf);
        return;
    }
    DlNvmeApi::DmaFree(buf);
}

Result HostDataOpNvme::DeviceToLocal(void *dest, const void *src, uint64_t size) noexcept
{
    return DlHybridApi::Memcpy(dest, size, src, size, ACL_MEMCPY_DEVICE_TO_HOST);
}

Result HostDataOpNvme::LocalToDevice(void *dest, const void *src, uint64_t size) noexcept
{
    return DlHybridApi::Memcpy(dest, size, src, size, ACL_MEMCPY_HOST_TO_DEVICE);
}

Result HostDataOpNvme::CopyCore(void *localPtr, uint64_t gva, uint64_t size, bool isWrite,
                                bool localIsDevice) noexcept
{
    HybmNvmeExtent extent;
    if (!HybmNvmeExtentRegistry::GetInstance().Lookup(gva, size, extent)) {
        BM_LOG_ERROR("gva 0x" << std::hex << gva << std::dec << " size " << size
                              << " is not a registered nvme extent");
        return BM_INVALID_PARAM;
    }

    auto ret = EnsureReady();
    if (ret != BM_OK) {
        return ret;
    }

    NvmeEndpointPtr endpoint;
    ret = GetEndpoint(extent, endpoint);
    if (ret != BM_OK) {
        return ret;
    }

    auto *local = static_cast<char *>(localPtr);
    const uint64_t extentOffset = gva - extent.gvaBase;
    uint64_t done = 0;
    while (done < size) {
        const uint64_t n = std::min(chunkSize_, size - done);
        void *buf = AcquireBuf();
        if (buf == nullptr) {
            BM_LOG_ERROR("nvme staging buffer exhausted");
            return BM_MALLOC_FAILED;
        }
        if (isWrite) {
            if (localIsDevice) {
                ret = DeviceToLocal(buf, local + done, n);
            } else {
                (void)memcpy(buf, local + done, n);
                ret = BM_OK;
            }
            if (ret == BM_OK) {
                ret = SubmitRange(endpoint, extent, extentOffset + done, buf, n, true);
            }
        } else {
            ret = SubmitRange(endpoint, extent, extentOffset + done, buf, n, false);
            if (ret == BM_OK && localIsDevice) {
                ret = LocalToDevice(local + done, buf, n);
            } else if (ret == BM_OK) {
                (void)memcpy(local + done, buf, n);
            }
        }
        ReleaseBuf(buf);
        if (ret != BM_OK) {
            return ret;
        }
        done += n;
    }
    return BM_OK;
}

Result HostDataOpNvme::DataCopy(hybm_copy_params &params, hybm_data_copy_direction direction,
                                const ExtOptions &options) noexcept
{
    (void)options;
    if (!inited_ || params.src == nullptr || params.dest == nullptr || params.dataSize == 0) {
        return BM_INVALID_PARAM;
    }
    if (!IsSupportedDirection(direction)) {
        return BM_INVALID_PARAM;
    }

    const bool isWrite = direction == HYBM_LOCAL_HOST_TO_GLOBAL_NVME || direction == HYBM_LOCAL_DEVICE_TO_GLOBAL_NVME;
    const bool localIsDevice =
        direction == HYBM_LOCAL_DEVICE_TO_GLOBAL_NVME || direction == HYBM_GLOBAL_NVME_TO_LOCAL_DEVICE;
    void *localPtr = isWrite ? params.src : params.dest;
    const uint64_t gva = reinterpret_cast<uint64_t>(isWrite ? params.dest : params.src);

    std::lock_guard<std::mutex> guard(ioMutex_);
    return CopyCore(localPtr, gva, params.dataSize, isWrite, localIsDevice);
}

Result HostDataOpNvme::DataCopyAsync(hybm_copy_params &params, hybm_data_copy_direction direction,
                                     const ExtOptions &options) noexcept
{
    /* batch 2: executed synchronously, the wait contract is preserved (Wait is a no-op
     * drain); deep async pipelining is a follow-up optimization */
    auto ret = DataCopy(params, direction, options);
    return ret == BM_OK ? 0 : ret;
}

Result HostDataOpNvme::BatchDataCopy(hybm_batch_copy_params &params, hybm_data_copy_direction direction,
                                     const ExtOptions &options) noexcept
{
    (void)options;
    if (!inited_ || params.sources == nullptr || params.destinations == nullptr || params.dataSizes == nullptr ||
        params.batchSize == 0) {
        return BM_INVALID_PARAM;
    }
    if (!IsSupportedDirection(direction)) {
        return BM_INVALID_PARAM;
    }

    const bool isWrite = direction == HYBM_LOCAL_HOST_TO_GLOBAL_NVME || direction == HYBM_LOCAL_DEVICE_TO_GLOBAL_NVME;
    const bool localIsDevice =
        direction == HYBM_LOCAL_DEVICE_TO_GLOBAL_NVME || direction == HYBM_GLOBAL_NVME_TO_LOCAL_DEVICE;

    std::lock_guard<std::mutex> guard(ioMutex_);
    for (uint32_t i = 0; i < params.batchSize; ++i) {
        void *localPtr = isWrite ? params.sources[i] : params.destinations[i];
        const uint64_t gva = reinterpret_cast<uint64_t>(isWrite ? params.destinations[i] : params.sources[i]);
        auto ret = CopyCore(localPtr, gva, params.dataSizes[i], isWrite, localIsDevice);
        if (ret != BM_OK) {
            BM_LOG_ERROR("nvme batch copy item " << i << " failed, ret:" << ret);
            return ret;
        }
    }
    return BM_OK;
}

Result HostDataOpNvme::Wait(int32_t waitId) noexcept
{
    (void)waitId;
    /* the IO path polls its own qpairs to completion; drain stragglers defensively */
    std::vector<NvmeEndpointPtr> snapshot;
    {
        std::lock_guard<std::mutex> guard(endpointsMutex_);
        for (auto &entry : endpoints_) {
            snapshot.push_back(entry.second);
        }
    }
    for (auto &endpoint : snapshot) {
        for (auto *qpair : endpoint->qpairs) {
            (void)DlNvmeApi::ProcessCompletions(qpair);
        }
    }
    return BM_OK;
}

} // namespace mf
} // namespace ock
