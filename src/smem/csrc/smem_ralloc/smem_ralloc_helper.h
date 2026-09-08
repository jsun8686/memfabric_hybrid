/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#ifndef MF_HYBRID_SMEM_RALLOC_HELPER_H
#define MF_HYBRID_SMEM_RALLOC_HELPER_H

#include <cstdint>
#include <algorithm>

#include "hybm_def.h"
#include "smem_ralloc_def.h"

namespace ock {
namespace smem {
class SmemRallocHelper {
public:
    static inline hybm_data_op_type TransHybmDataOpType(smem_ralloc_data_op_type smemRallocDataOpType)
    {
        uint32_t resultOpType = 0;
        if (smemRallocDataOpType & SMEMRA_DATA_OP_SDMA) {
            resultOpType |= HYBM_DOP_TYPE_SDMA;
        }

        if (smemRallocDataOpType & SMEMRA_DATA_OP_DEVICE_RDMA) {
            resultOpType |= HYBM_DOP_TYPE_DEVICE_RDMA;
        }

        if (smemRallocDataOpType & SMEMRA_DATA_OP_HOST_RDMA) {
            resultOpType |= HYBM_DOP_TYPE_HOST_RDMA;
        }

        if (smemRallocDataOpType & SMEMRA_DATA_OP_HOST_URMA) {
            resultOpType |= HYBM_DOP_TYPE_HOST_URMA;
        }

        if (smemRallocDataOpType & SMEMRA_DATA_OP_HOST_TCP) {
            resultOpType |= HYBM_DOP_TYPE_HOST_TCP;
        }

        if (smemRallocDataOpType & SMEMRA_DATA_OP_HOST_SHM) {
            resultOpType |= HYBM_DOP_TYPE_HOST_SHM;
        }

        return static_cast<hybm_data_op_type>(resultOpType);
    }

    static inline uint32_t TransSmemDataOpType(hybm_data_op_type hybmDataOpType)
    {
        uint32_t resultOpType = 0;
        if (hybmDataOpType & HYBM_DOP_TYPE_SDMA) {
            resultOpType |= SMEMRA_DATA_OP_SDMA;
        }

        if (hybmDataOpType & HYBM_DOP_TYPE_DEVICE_RDMA) {
            resultOpType |= SMEMRA_DATA_OP_DEVICE_RDMA;
        }

        if (hybmDataOpType & HYBM_DOP_TYPE_HOST_RDMA) {
            resultOpType |= SMEMRA_DATA_OP_HOST_RDMA;
        }

        if (hybmDataOpType & HYBM_DOP_TYPE_HOST_URMA) {
            resultOpType |= SMEMRA_DATA_OP_HOST_URMA;
        }

        if (hybmDataOpType & HYBM_DOP_TYPE_HOST_TCP) {
            resultOpType |= SMEMRA_DATA_OP_HOST_TCP;
        }

        if (hybmDataOpType & HYBM_DOP_TYPE_HOST_SHM) {
            resultOpType |= SMEMRA_DATA_OP_HOST_SHM;
        }

        return resultOpType;
    }

    static inline void TransHybmTlsOption(const smem_ralloc_tls_config &src, hybm_tls_option &dst)
    {
        dst.tlsEnable = src.tlsEnable;
        std::copy_n(src.caPath, SMEM_RALLOC_TLS_PATH_SIZE, dst.caPath);
        std::copy_n(src.crlPath, SMEM_RALLOC_TLS_PATH_SIZE, dst.crlPath);
        std::copy_n(src.certPath, SMEM_RALLOC_TLS_PATH_SIZE, dst.certPath);
        std::copy_n(src.keyPath, SMEM_RALLOC_TLS_PATH_SIZE, dst.keyPath);
        std::copy_n(src.keyPassPath, SMEM_RALLOC_TLS_PATH_SIZE, dst.keyPassPath);
        std::copy_n(src.packagePath, SMEM_RALLOC_TLS_PATH_SIZE, dst.packagePath);
        std::copy_n(src.decrypterLibPath, SMEM_RALLOC_TLS_PATH_SIZE, dst.decrypterLibPath);
    }
};
} // namespace smem
} // namespace ock

#endif // MF_HYBRID_SMEM_RALLOC_HELPER_H
