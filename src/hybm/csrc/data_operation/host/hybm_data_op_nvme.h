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
#ifndef MF_HYBRID_HYBM_DATA_OP_NVME_H
#define MF_HYBRID_HYBM_DATA_OP_NVME_H

#include <atomic>
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
 * pointers / local device VA and remote LBA extents through a WINDOWED pipeline:
 *   write: user -> spdk_dma staging -> NVMe-oF write (CQE == data on SSD)
 *   read:  NVMe-oF read -> staging -> local; a LOCAL_DEVICE endpoint additionally rides
 *          SDMA (AclrtMemcpy) for the local host<->HBM last hop.
 * Sub-LBA heads/tails ride a read-modify-write bounce (two rounds: the read halves share
 * the window with the mid writes, the merged head/tail writes are re-submitted as round
 * two), so byte-granular GVA ranges work.
 * Lifecycles are staged EARLY instead of lazily on the first IO (fail-fast semantics):
 *   pool create -> Initialize(): spdk dlopen + dpdk EAL (hard fail)
 *   extent import -> Warmup(): ctrlr connect + io qpair pre-creation (hard fail, rolled
 *                             back by the registry on error)
 * Concurrency: one window owns one qpair slot for its whole duration (spdk qpairs are not
 * thread-safe); the slot pool is the concurrency ceiling, there is NO global IO lock. */
class HostDataOpNvme : public DataOperator {
public:
    /* trstring: NVMe-oF transport declared by the pool op bit ("tcp"/"rdma");
     * no env override -- the op bit is the sole source of truth and the FAR side
     * verifies it against the target listener */
    explicit HostDataOpNvme(uint32_t rankId, const std::string &trstring) noexcept;
    ~HostDataOpNvme() override;

    Result Initialize() noexcept override;
    void UnInitialize() noexcept override;

    Result DataCopy(hybm_copy_params &params, hybm_data_copy_direction direction,
                    const ExtOptions &options) noexcept override;
    Result DataCopyAsync(hybm_copy_params &params, hybm_data_copy_direction direction,
                         const ExtOptions &options) noexcept override;
    Result BatchDataCopy(hybm_batch_copy_params &params, hybm_data_copy_direction direction,
                         const ExtOptions &options) noexcept override;
    /* address model (deliberately a no-op): caller memory is NATIVE HVA/DVA (never part of
     * the GVA space) and the NVMe GVA side is always remote disk media -- the local VA
     * manager never holds a mapping for it, so a best-effort GVA->HVA/DVA translation
     * would be an identity every time. Compare host_rdma, whose GLOBAL_HOST side CAN
     * resolve to a local pool block and needs the translation. */
    void TransformVa(void *&src, void *&dst, hybm_data_copy_direction direction) noexcept override
    {
        (void)src;
        (void)dst;
        (void)direction;
        return;
    }
    Result Wait(int32_t waitId) noexcept override;

    void CleanUp() noexcept override;

    /* connect the ctrlr + pre-create the io qpair slots of one extent (idempotent);
     * fired by the extent registry at import time -- HARD FAIL semantics: an unreachable
     * target aborts the import/JOIN instead of surfacing on the first IO */
    Result Warmup(const HybmNvmeExtent &extent) noexcept;

private:
    /* completion state of one command. Deliberately NOT atomic: spdk fires the callback
     * inline from ProcessCompletions, i.e. on the very thread that polls the window --
     * submit and reap never race. Revisit if a future async pipeline moves completions
     * to another thread. */
    struct IoCtx {
        int32_t done{0};
        int32_t status{0}; /* 0 = success, 1 = cpl error (SC/SCT != 0) */
    };

    /* one NVMe command inside the window pipeline */
    struct NvmeCmd {
        uint64_t lba{0};
        uint32_t lbaCount{0};
        bool isWrite{false};
        bool rmw{false};        /* sector-sized: 512B pool; direct mid IO uses chunk blocks */
        bool pendingWrite{false}; /* write path RMW read-half: reaped -> merge -> round 2 */
        char *staging{nullptr}; /* dma buffer (lbaCount * 512), acquired on submit */
        char *userPtr{nullptr}; /* local-side bytes backing this command */
        uint32_t userOff{0};    /* user bytes offset inside staging (RMW: sub-sector) */
        uint32_t userLen{0};    /* user bytes length carried by this command */
        bool localIsDevice{false};
        bool finished{false};
        bool released{false};   /* staging ownership handed over (pool/reap) */
        IoCtx ctx{};
    };

    /* the concurrency boundary: spdk qpairs are not thread-safe, so exactly one window
     * holds a slot (with its mutex) for the whole submit+poll duration */
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
        uint32_t maxXferBytes{128U * 1024U}; /* per-command cap (ctrlr MDTS), spdk does not split */
    };
    using NvmeEndpointPtr = std::shared_ptr<NvmeEndpoint>;

    struct CopyItem {
        char *local{nullptr};
        uint64_t gva{0};
        uint64_t size{0};
    };

    bool IsSupportedDirection(hybm_data_copy_direction direction) const noexcept;
    /* one-time spdk load + EAL init + post-init (idempotent); called from Initialize
     * (pool create, hard fail) and defensively from Warmup */
    Result EnsureReady() noexcept;
    /* cache lookup only: endpoints must exist by the time IO runs (Warmup at import);
     * a miss is BM_NOT_CONNECTED -- no lazy connect fallback by design */
    Result GetEndpoint(const HybmNvmeExtent &extent, NvmeEndpointPtr &endpoint) noexcept;
    SlotLease AcquireSlot(const NvmeEndpointPtr &endpoint) noexcept;
    /* expand one byte range of an extent into commands: mid full-sector IO capped by
     * min(MDTS, chunk), sub-sector head/tail as RMW read-halves (write) / slice-outs
     * (read); staging buffers are NOT taken here -- the window fills them lazily */
    void BuildCmds(const NvmeEndpointPtr &endpoint, const HybmNvmeExtent &extent, uint64_t offset,
                   char *local, uint64_t len, bool isWrite, bool localIsDevice,
                   std::vector<NvmeCmd> &cmds) noexcept;
    /* acquire staging + stage user bytes in, then submit one command */
    Result PrepareAndSubmit(const NvmeEndpointPtr &endpoint, QpairSlot *slot, NvmeCmd &cmd) noexcept;
    /* completions of one round: reap done commands (read stage-out / RMW merge into
     * nextRound), stop issuing on the first error but keep draining what is in flight */
    Result SubmitWindowed(const NvmeEndpointPtr &endpoint, QpairSlot *slot, std::vector<NvmeCmd> &cmds,
                          std::vector<NvmeCmd> *nextRound) noexcept;
    Result ReapCmd(NvmeCmd &cmd, std::vector<NvmeCmd> *nextRound) noexcept;
    /* release staging of commands that never got reaped (error paths) */
    void ReleaseCmds(std::vector<NvmeCmd> &cmds) noexcept;
    /* shared engine of DataCopy / BatchDataCopy: items are grouped per endpoint, each
     * group runs one two-round window; a failed group is recorded and the rest still run */
    Result RunItems(std::vector<CopyItem> &items, bool isWrite, bool localIsDevice) noexcept;
    Result DeviceToLocal(void *dest, const void *src, uint64_t size) noexcept;
    Result LocalToDevice(void *dest, const void *src, uint64_t size) noexcept;
    Result CopyUserOut(const NvmeCmd &cmd) noexcept; /* staging -> user (host memcpy / H2D) */
    Result CopyUserIn(const NvmeCmd &cmd) noexcept;  /* user -> staging (host memcpy / D2H) */

    /* lazy dma pools, two size classes: chunk blocks (mid IO) and 512B sectors (RMW);
     * free lists are capped, overflow blocks are freed back to spdk immediately */
    void *AcquireBuf(bool sector) noexcept;
    void ReleaseBuf(void *buf, bool sector) noexcept;

    static void IoDone(void *ctx, const void *cpl) noexcept;

    bool inited_{false};
    bool ready_{false};
    uint32_t rankId_{0};
    uint64_t chunkSize_{4ULL * 1024 * 1024}; /* staging chunk (per-segment cap), env MF_NVME_BUF_MB */
    uint32_t qdepth_{16U};                   /* in-flight commands per window, env MF_NVME_QDEPTH */
    uint32_t bufSlots_{16U};                 /* pool depth per size class, env MF_NVME_BUF_SLOTS (=QDEPTH) */
    uint32_t qpairsPerCtrlr_{4U};            /* slot count = concurrent windows, env MF_NVME_QPAIRS */
    std::string trstring_;                   /* NVMe-oF transport, from the pool op bit ("rdma"/"tcp") */

    std::mutex endpointsMutex_;
    std::map<std::string, NvmeEndpointPtr> endpoints_; /* key: nqn|traddr|trsvcid|nsid */

    std::mutex bufMutex_;
    std::vector<void *> freeSegBufs_;    /* chunkSize_ blocks */
    std::vector<void *> freeSectorBufs_; /* 512B blocks */
};

} // namespace mf
} // namespace ock

#endif // MF_HYBRID_HYBM_DATA_OP_NVME_H
