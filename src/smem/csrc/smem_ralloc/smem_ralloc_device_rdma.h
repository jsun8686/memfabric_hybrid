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

/* Runtime loader of the install-time built device RDMA kernel library
 * (libmf_smem_ralloc_device_rdma.so, compiled by bisheng from smem_ralloc_device_kernel.cpp,
 * same packaging mechanism as libmf_hybm_copy_extend.so). The ralloc layer must not depend on
 * the bm layer, so this loader vendors the dlopen pattern with plain libc calls instead of
 * reusing the bm FileUtil helpers.
 */
#ifndef MEMFABRIC_HYBRID_SMEM_RALLOC_DEVICE_RDMA_H
#define MEMFABRIC_HYBRID_SMEM_RALLOC_DEVICE_RDMA_H

#include <cstdint>
#include <mutex>

namespace ock {
namespace smem {

using SmemRallocDeviceWriteRunFunc = void (*)(uint32_t entityId, uint32_t dstRank, void *dst, void *src,
                                              uint64_t len, uint32_t iters, uint32_t dim, void *stream);

using SmemRallocDeviceReadRunFunc = void (*)(uint32_t entityId, uint32_t srcRank, void *dst, void *src,
                                             uint64_t len, uint32_t iters, uint32_t dim, void *stream);

class DlSmemRallocDeviceApi {
public:
    /* dlopen the kernel library lazily, thread safe, returns true when the submit entries are ready */
    static bool TryLoadLibrary();

    static void CleanupLibrary();

    static bool IsLoaded()
    {
        return gLoaded;
    }

    static SmemRallocDeviceWriteRunFunc GetWriteRunSubmit()
    {
        return pWriteRunSubmit;
    }

    static SmemRallocDeviceReadRunFunc GetReadRunSubmit()
    {
        return pReadRunSubmit;
    }

private:
    static bool gLoaded;
    static std::mutex gMutex;
    static void *libHandle;
    static SmemRallocDeviceWriteRunFunc pWriteRunSubmit;
    static SmemRallocDeviceReadRunFunc pReadRunSubmit;
};

} // namespace smem
} // namespace ock

#endif // MEMFABRIC_HYBRID_SMEM_RALLOC_DEVICE_RDMA_H
