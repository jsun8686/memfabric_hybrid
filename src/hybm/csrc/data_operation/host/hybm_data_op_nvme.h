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
#ifndef MF_HYBRID_HYBM_DATA_OP_NVME_H
#define MF_HYBRID_HYBM_DATA_OP_NVME_H

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "dl_nvme_api.h"
#include "hybm_data_operator.h"
#include "hybm_nvme_segment.h"

namespace ock {
namespace mf {

/* NVMe-oF data operator (host initiator). Consumes HybmNvmeExtentRegistry entries written
 * by the NVMe segment (owner alloc / member import) and moves bytes between local host
 * pointers / local device VA and remote LBA extents:
 *   write: local -> spdk_dma staging buffer -> NVMe-oF write (CQE == data on SSD)
 *   read:  NVMe-oF read -> staging -> local; a LOCAL_DEVICE endpoint additionally rides
 *          SDMA (AclrtMemcpy) for the local host<->HBM last hop.
 * Sub-LBA tails go through a read-modify-write bounce, so byte-granular GVA ranges work.
 * Chunked by the staging buffer size; each chunk is submitted and polled to completion
 * (batch 2: correctness first, deep pipelining is a follow-up optimization). */
class HostDataOpNvme : public DataOperator {
public:
    explicit HostDataOpNvme(uint32_t rankId) noexcept;
    ~HostDataOpNvme() override;

    Result Initialize() noexcept override;
    void UnInitialize() noexcept override;

    Result DataCopy(hybm_copy_params &params, hybm_data_copy_direction direction,
                    const ExtOptions &options) noexcept override;
    Result DataCopyAsync(hybm_copy_params &params, hybm_data_copy_direction direction,
                         const ExtOptions &options) noexcept override;
    Result BatchDataCopy(hybm_batch_copy_params &params, hybm_data_copy_direction direction,
                         const ExtOptions &options) noexcept override;
    void TransformVa(void *&src, void *&dst, hybm_data_copy_direction direction) noexcept override
    {
        return;
    }
    Result Wait(int32_t waitId) noexcept override;

    void CleanUp() noexcept override;

private:
    struct NvmeEndpoint {
        void *ctrlr{nullptr};
        void *ns{nullptr};
        std::vector<void *> qpairs;
        uint32_t nextQpair{0};
        uint32_t maxXferBytes{128U * 1024U}; /* per-command cap (ctrlr MDTS), spdk does not split */
    };
    using NvmeEndpointPtr = std::shared_ptr<NvmeEndpoint>;

    bool IsSupportedDirection(hybm_data_copy_direction direction) const noexcept;
    /* lazy one-time loader + env post-init + staging pool */
    Result EnsureReady() noexcept;
    Result GetEndpoint(const HybmNvmeExtent &extent, NvmeEndpointPtr &endpoint) noexcept;
    /* one [offset, offset+len) byte range inside an extent, buf points at the staged bytes */
    Result SubmitRange(const NvmeEndpointPtr &endpoint, const HybmNvmeExtent &extent, uint64_t offset,
                       const void *buf, uint64_t len, bool isWrite) noexcept;
    /* n bytes at offInSector inside one sector: write = read-modify-write, read = slice out */
    Result RmwSector(const NvmeEndpointPtr &endpoint, uint64_t sectorLba, uint32_t offInSector, uint32_t n,
                     char *ioBytes, bool isWrite) noexcept;
    Result SubmitAndWait(const NvmeEndpointPtr &endpoint, void *qpair, bool isWrite, uint64_t lba,
                         uint32_t lbaCount, void *buf) noexcept;
    void *NextQpair(const NvmeEndpointPtr &endpoint) noexcept;
    Result CopyCore(void *localPtr, uint64_t gva, uint64_t size, bool isWrite, bool localIsDevice) noexcept;
    Result DeviceToLocal(void *dest, const void *src, uint64_t size) noexcept;
    Result LocalToDevice(void *dest, const void *src, uint64_t size) noexcept;

    void *AcquireBuf() noexcept;
    void ReleaseBuf(void *buf) noexcept;

    static void IoDone(void *ctx, const void *cpl) noexcept;

    bool inited_{false};
    bool ready_{false};
    uint32_t rankId_{0};
    uint64_t chunkSize_{4ULL * 1024 * 1024}; /* staging chunk, env MF_NVME_BUF_MB */
    uint32_t bufSlots_{4U};                  /* staging pool depth, env MF_NVME_BUF_SLOTS */
    uint32_t qpairsPerCtrlr_{2U};            /* env MF_NVME_QPAIRS */
    std::string trstring_{"rdma"};           /* NVMe-oF transport, env MF_NVME_TRSTRING ("rdma"/"tcp") */
    char *scratch_{nullptr};                 /* one 512B RMW sector */

    std::mutex ioMutex_;                     /* serializes the IO path (batch 2) */
    std::mutex endpointsMutex_;
    std::map<std::string, NvmeEndpointPtr> endpoints_; /* key: nqn|traddr|trsvcid|nsid */

    std::mutex bufMutex_;
    std::vector<void *> freeBufs_;
};

} // namespace mf
} // namespace ock

#endif // MF_HYBRID_HYBM_DATA_OP_NVME_H
