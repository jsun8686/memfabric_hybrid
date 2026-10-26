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

#ifndef MF_HYBRID_HYBM_DATA_OP_NVME_H
#define MF_HYBRID_HYBM_DATA_OP_NVME_H

#include <string>

#include "hybm_data_operator.h"
#include "hybm_nvme_segment.h"

namespace ock {
namespace mf {

/* NVMe-oF data operator: pure ORCHESTRATION layer (mirrors HostDataOpRDMA's shape --
 * direction classification + batch grouping, transmission delegated to the transport).
 * The whole transmission domain (SPDK env, ctrlr connections, qpair leases, staging
 * pools, MR registration, MDTS/RMW/window engine) lives in NvmeTransportManager, the
 * process-wide singleton sub-transport -- pools of one process share its connections.
 *
 * Layering facts that used to live here and are now transport invariants:
 *  - pool create -> Initialize(): spdk dlopen + dpdk EAL preload, hard fail (A8a)
 *  - extent import -> warmup hook -> transport Warmup: connect + pre-created qpairs,
 *    HARD FAIL (the registry rolls the import back); GetEndpoint refuses lazy connects
 *  - window engine: qdepth-deep submit/poll overlap, two-round RMW, per-command
 *    zero-copy hits (MR table QueryHasRegistered)
 *  - the op itself holds no locks, no pools, no endpoint state: every DataCopy is
 *    stateless routing into transport::RunItems */
class HostDataOpNvme : public DataOperator {
public:
    /* trstring: NVMe-oF transport declared by the pool op bit ("tcp"/"rdma");
     * the sole source of truth is the op bit, there is no env override */
    HostDataOpNvme(uint32_t rankId, const std::string &trstring) noexcept;
    ~HostDataOpNvme() noexcept override;

    Result Initialize() noexcept override;
    void UnInitialize() noexcept override;
    void CleanUp() noexcept override;

    Result DataCopy(hybm_copy_params &params, hybm_data_copy_direction direction,
                    const ExtOptions &options) noexcept override;
    Result BatchDataCopy(hybm_batch_copy_params &params, hybm_data_copy_direction direction,
                         const ExtOptions &options) noexcept override;
    Result DataCopyAsync(hybm_copy_params &params, hybm_data_copy_direction direction,
                         const ExtOptions &options) noexcept override;
    Result Wait(int32_t waitId) noexcept override;

    /* user address model note: local endpoints are native HVA/DVA (never GVAs of this
     * pool) and the NVMe GVA side is always remote disk media -- the local VA
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

    /* connect the ctrlr + pre-create the io qpair slots of one extent (idempotent);
     * fired by the extent registry at import time -- HARD FAIL semantics: an unreachable
     * target aborts the import/JOIN instead of surfacing on the first IO */
    Result Warmup(const HybmNvmeExtent &extent) noexcept;

    bool IsSupportedDirection(hybm_data_copy_direction direction) const noexcept;

private:
    bool inited_{false};
    uint32_t rankId_{0};
    std::string trstring_; /* NVMe-oF transport, from the pool op bit ("rdma"/"tcp") */
};

} // namespace mf
} // namespace ock

#endif // MF_HYBRID_HYBM_DATA_OP_NVME_H
