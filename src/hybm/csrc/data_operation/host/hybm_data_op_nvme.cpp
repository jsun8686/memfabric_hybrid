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

#include "hybm_data_op_nvme.h"

#include <vector>

#include "hybm_ptracer.h"
#include "nvme_transport_manager.h"

namespace ock {
namespace mf {

HostDataOpNvme::HostDataOpNvme(uint32_t rankId, const std::string &trstring) noexcept
    : rankId_(rankId), trstring_(trstring)
{
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
    /* pool create time: bring the whole spdk stack up NOW and fail the create when the
     * env is broken (a missing spdk install is a deployment error, the earlier reported
     * the better -- historically this only surfaced on the first IO) */
    TP_TRACE_BEGIN(TP_HYBM_NVME_PRELOAD);
    auto ret = transport::NvmeTransportManager::GetInstance()->EnsureEnv();
    TP_TRACE_END(TP_HYBM_NVME_PRELOAD, ret);
    if (ret != BM_OK) {
        BM_LOG_ERROR("nvme data operator spdk preload failed, ret:" << ret);
        return ret;
    }
    inited_ = true;
    /* warmup hook: the extent registry pre-connects every imported extent (hard fail on
     * error, rolled back by the registry) so the first IO starts with a live endpoint.
     * The hook stays HERE (mm layer keeps calling the operator), the connect itself is
     * transport work -- the shared singleton deduplicates pools of one process. */
    HybmNvmeExtentRegistry::GetInstance().SetWarmupCallback(
        [this](const HybmNvmeExtent &extent) { return Warmup(extent); });
    BM_LOG_INFO("nvme data operator initialized, rank:" << rankId_ << ", trstring:" << trstring_);
    return BM_OK;
}

void HostDataOpNvme::UnInitialize() noexcept
{
    /* drop the warmup hook first: it captures `this` and would dangle after teardown.
     * The transport singleton outlives the operator (process-wide shared connections);
     * it is NOT torn down here -- its CloseDevice is driven by pool teardown policy. */
    HybmNvmeExtentRegistry::GetInstance().SetWarmupCallback(nullptr);
    inited_ = false;
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

Result HostDataOpNvme::Warmup(const HybmNvmeExtent &extent) noexcept
{
    return transport::NvmeTransportManager::GetInstance()->Warmup(extent, trstring_);
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

    TP_TRACE_BEGIN(TP_HYBM_NVME_COPY_TOTAL);
    std::vector<transport::NvmeTransportManager::NvmeCopyItem> items;
    transport::NvmeTransportManager::NvmeCopyItem item;
    item.local = static_cast<char *>(isWrite ? params.src : params.dest);
    item.gva = reinterpret_cast<uint64_t>(isWrite ? params.dest : params.src);
    item.size = params.dataSize;
    items.push_back(item);
    const Result ret = transport::NvmeTransportManager::GetInstance()->RunItems(items, isWrite, localIsDevice);
    TP_TRACE_END(TP_HYBM_NVME_COPY_TOTAL, ret);
    return ret;
}

Result HostDataOpNvme::DataCopyAsync(hybm_copy_params &params, hybm_data_copy_direction direction,
                                     const ExtOptions &options) noexcept
{
    /* executed synchronously, the wait contract is preserved (Wait is a no-op drain);
     * deep async pipelining is a follow-up optimization */
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

    TP_TRACE_BEGIN(TP_HYBM_NVME_COPY_TOTAL);
    std::vector<transport::NvmeTransportManager::NvmeCopyItem> items;
    items.reserve(params.batchSize);
    for (uint32_t i = 0; i < params.batchSize; ++i) {
        transport::NvmeTransportManager::NvmeCopyItem item;
        item.local = static_cast<char *>(isWrite ? params.sources[i] : params.destinations[i]);
        item.gva = reinterpret_cast<uint64_t>(isWrite ? params.destinations[i] : params.sources[i]);
        item.size = params.dataSizes[i];
        items.push_back(item);
    }
    /* the WHOLE batch expands into one command sequence per endpoint: small IOs ride the
     * same window and pipeline together instead of paying a serial round-trip each */
    const Result ret = transport::NvmeTransportManager::GetInstance()->RunItems(items, isWrite, localIsDevice);
    TP_TRACE_END(TP_HYBM_NVME_COPY_TOTAL, ret);
    return ret;
}

Result HostDataOpNvme::Wait(int32_t waitId) noexcept
{
    (void)waitId;
    return transport::NvmeTransportManager::GetInstance()->Drain();
}

} // namespace mf
} // namespace ock
