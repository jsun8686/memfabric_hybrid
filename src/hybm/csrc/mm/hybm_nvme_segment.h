/*
 * Copyright: (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
*/
#ifndef MF_HYBRID_HYBM_NVME_SEGMENT_H
#define MF_HYBRID_HYBM_NVME_SEGMENT_H

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "hybm_define.h"
#include "hybm_mem_common.h"
#include "hybm_mem_segment.h"

namespace ock {
namespace mf {

/* serialized slice exchange info of an NVMe extent, prefix-compatible with HostSdmaExportInfo
 * (magic@0 / segmentType@8 / version@12), padded to the unified 192B exchange size */
struct NvmeExportInfo {
    uint64_t magic{NVME_SLICE_EXPORT_INFO_MAGIC};
    uint32_t segmentType{SEGMENT_TYPE_NVME};
    uint32_t version{EXPORT_INFO_VERSION};
    uint64_t gva{0};        /* base gva of the extent inside the owner rank's NVMe band slot */
    uint32_t sliceIndex{0}; /* slice index on the owner segment */
    uint32_t ownerRank{0};  /* rank which owns (contributes) the extent */
    uint32_t blockId{0};    /* opaque per-pool extent id, unique on the contributor */
    uint32_t nsid{0};       /* namespace id on the contributor's nvmf target */
    uint32_t trsvcid{0};    /* NVMe-oF listener port of the contributor's nvmf target */
    uint32_t reserved{0};
    uint64_t lbaBase{0}; /* first LBA of the extent, 512B units */
    uint64_t lbaLen{0};  /* extent length in LBAs (512B units), >= vsize rounded to granularity */
    uint64_t vsize{0};   /* VA size of the extent in bytes (what callers address via gva) */
    char nqn[64]{};   /* subsystem NQN of the contributor's nvmf target */
    char traddr[46]{}; /* listener address (IP string) of the contributor's nvmf target */
    uint8_t tailPad[UNIFIED_EXCHANGE_SEG_INFO_SIZE - 72U - 64U - 46U]{}; /* 10B -> 192B total */
};
static_assert(sizeof(NvmeExportInfo) == UNIFIED_EXCHANGE_SEG_INFO_SIZE,
              "NvmeExportInfo must be 192 bytes, compatible with the unified exchange size");
static_assert(offsetof(NvmeExportInfo, segmentType) == SEGMENT_TYPE_OFFSET, "segmentType offset mismatch!");

/* runtime view of one NVMe extent, shared between the segment (writer: owner alloc / member
 * import) and the NVMe data operator (reader: gva -> LBA translation, see hybm_data_op_nvme) */
struct HybmNvmeExtent {
    uint64_t gvaBase{0};
    uint64_t size{0};
    uint32_t ownerRank{0};
    uint32_t blockId{0};
    uint32_t nsid{0};
    uint32_t trsvcid{0};
    uint64_t lbaBase{0};
    uint64_t lbaLen{0};
    char nqn[64]{};
    char traddr[46]{};
};

/* process-wide gva -> extent registry: the single source of truth for the NVMe data plane */
class HybmNvmeExtentRegistry {
public:
    static HybmNvmeExtentRegistry &GetInstance();

    HybmNvmeExtentRegistry(const HybmNvmeExtentRegistry &) = delete;
    HybmNvmeExtentRegistry &operator=(const HybmNvmeExtentRegistry &) = delete;

    void Register(const HybmNvmeExtent &extent);
    void UnregisterByGva(uint64_t gvaBase);
    void UnregisterByRank(uint32_t rank);
    /* resolve the extent containing [gva, gva + size), false when gva is not NVMe media */
    bool Lookup(uint64_t gva, uint64_t size, HybmNvmeExtent &out) const;
    void Clear();

private:
    HybmNvmeExtentRegistry() = default;

    mutable std::mutex mutex_;
    std::map<uint64_t, HybmNvmeExtent> extents_; /* keyed by gvaBase */
};

/* admin-plane client of the local nvmf target (pure interface: UT injects a mock, production
 * uses SpdkNvmfAdminClient talking SPDK JSON-RPC over a unix socket). The data plane never
 * goes through here -- members' NVMe-oF IO connects straight to the target. */
class HybmNvmeAdminClient {
public:
    virtual ~HybmNvmeAdminClient() = default;

    /* expose [lbaOffset, lbaOffset + lbaLen) as a fresh namespace; returns its nsid */
    virtual Result AddNs(uint64_t lbaOffset, uint64_t lbaLen, uint32_t &nsid) = 0;
    /* destroy the namespace of a released extent (idempotent) */
    virtual Result RemoveNs(uint32_t nsid) = 0;
    /* cheap liveness probe: false when the target is unreachable (capacity report drops to 0,
     * placement self-heals on the next extend) */
    virtual bool Healthy() = 0;
};

/* SPDK JSON-RPC implementation over a unix socket (newline-delimited JSON requests).
 * Identity (nqn/traddr/trsvcid, rpc socket path, base bdev, LBA base) comes from env:
 *   MF_NVME_NQN        subsystem NQN of the local nvmf target (default: auto per entity)
 *   MF_NVME_TRADDR      listener IP string the initiators should use (default: first non-lo)
 *   MF_NVME_TRSVCID     listener port (default 4420)
 *   MF_NVME_RPC_SOCK    SPDK rpc socket path (default /var/tmp/spdk.sock)
 *   MF_NVME_BASE_BDEV   base bdev name backing the NVMe pool (default "Nvme0n1")
 *   MF_NVME_LBA_BASE    first LBA this contributor may hand out (default 0; disjoint ranges
 *                       are required when several FAR ranks share one node's target) */
class SpdkNvmfAdminClient : public HybmNvmeAdminClient {
public:
    SpdkNvmfAdminClient() = default;
    ~SpdkNvmfAdminClient() override = default;

    Result AddNs(uint64_t lbaOffset, uint64_t lbaLen, uint32_t &nsid) override;
    Result RemoveNs(uint32_t nsid) override;
    bool Healthy() override;

    static std::string GetNqn();
    static std::string GetTraddr();
    static uint32_t GetTrsvcid();

private:
    /* one newline-terminated JSON-RPC roundtrip; false on transport failure */
    bool Roundtrip(const std::string &request, std::string &response);
};

/* the fourth MemSegment: NVMe block media. VA band reserved only (never materialized, never
 * MR-registered); AllocLocalMemory carves an LBA extent from the local target and exports the
 * nvmf identity so every pool member can connect its own initiator. */
class HybmNvmeSegment : public MemSegment {
public:
    /* client == nullptr -> default SpdkNvmfAdminClient; UT injects a mock here */
    explicit HybmNvmeSegment(const MemSegmentOptions &options, int eid,
                             std::shared_ptr<HybmNvmeAdminClient> client = nullptr);
    ~HybmNvmeSegment() override;

    Result ValidateOptions() noexcept override;
    Result ReserveMemorySpace(void **address) noexcept override;
    Result UnReserveMemorySpace() noexcept override;
    Result AllocLocalMemory(uint64_t size, MemSlicePtr &slice) noexcept override;
    Result RegisterMemory(const void *addr, uint64_t size, MemSlicePtr &slice) noexcept override;
    Result ReleaseSliceMemory(const MemSlicePtr &slice) noexcept override;
    Result Export(std::string &exInfo) noexcept override;
    Result Export(const MemSlicePtr &slice, std::string &exInfo) noexcept override;
    Result GetExportSliceSize(size_t &size) noexcept override;
    Result Import(const std::vector<std::string> &allExInfo, void *addresses[]) noexcept override;
    Result RemoveImported(const std::vector<uint32_t> &ranks) noexcept override;
    Result Mmap() noexcept override;
    Result Unmap() noexcept override;
    MemSlicePtr GetMemSlice(hybm_mem_slice_t slice, bool quiet) const noexcept override;
    bool MemoryInRange(const void *begin, uint64_t size) const noexcept override;

    hybm_mem_type GetMemoryType() const noexcept override
    {
        return HYBM_MEM_TYPE_NVME;
    }

private:
    /* first-fit carve inside [lbaBaseEnv_, lvaBaseEnv_ + capacity), returns byte offset */
    bool AllocLbaRange(uint64_t size, uint64_t &offsetOut);
    void FreeLbaRange(uint64_t offset, uint64_t size);
    void FillExtent(NvmeExportInfo &info) const;

    std::shared_ptr<HybmNvmeAdminClient> admin_;
    std::vector<NvmeExportInfo> imports_;          /* deserialized member-side extents */
    std::map<uint32_t, MemSliceStatus> slices_;    /* owner-side extents */
    std::map<uint32_t, NvmeExportInfo> sliceExtents_; /* sliceIndex -> extent detail (release path) */
    std::map<uint32_t, std::string> exportMap_;    /* sliceIndex -> serialized NvmeExportInfo */
    uint64_t granularity_{1ULL << 30};             /* LBA carving granularity, >= 2M, env-tunable */
    uint8_t *globalVirtualAddress_{nullptr};       /* band base (rankCnt * maxSize window) */
    uint64_t totalVirtualSize_{0UL};
    uint64_t allocatedSize_{0UL};                  /* bump cursor inside this rank's slot */
    uint64_t lbaBaseEnv_{0UL};                     /* first LBA this contributor may hand out */
    uint64_t lbaCapacity_{0UL};                    /* LBA bytes this contributor may hand out */
    std::map<uint64_t, uint64_t> freeLba_;         /* offset -> bytes, first-fit, coalesced */
    uint32_t nextBlockId_{1U};                     /* blockId generator (0 reserved) */
};
} // namespace mf
} // namespace ock

#endif // MF_HYBRID_HYBM_NVME_SEGMENT_H
