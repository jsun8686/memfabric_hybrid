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
#include <dlfcn.h>
#include <unistd.h>

#include <string>

#include "smem_logger.h"
#include "smem_ralloc_device_rdma.h"

namespace ock {
namespace smem {
bool DlSmemRallocDeviceApi::gLoaded = false;
std::mutex DlSmemRallocDeviceApi::gMutex;
void *DlSmemRallocDeviceApi::libHandle = nullptr;
SmemRallocDeviceWriteRunFunc DlSmemRallocDeviceApi::pWriteRunSubmit = nullptr;
SmemRallocDeviceReadRunFunc DlSmemRallocDeviceApi::pReadRunSubmit = nullptr;
SmemRallocDeviceBatchRunFunc DlSmemRallocDeviceApi::pBatchRunSubmit = nullptr;
SmemRallocDeviceDumpRunFunc DlSmemRallocDeviceApi::pDumpRunSubmit = nullptr;

static const char *SMEM_RALLOC_DEVICE_LIB_NAME = "libmf_smem_ralloc_device_rdma.so";

bool DlSmemRallocDeviceApi::TryLoadLibrary()
{
    std::unique_lock<std::mutex> guard(gMutex);
    if (gLoaded) {
        return true;
    }

    /* MEMFABRIC_HYBRID_EXTEND_LIB_PATH points at the runtime package lib root (lib64),
     * exported by set_env.sh and shared with the copy_extend loader */
    char *path = std::getenv("MEMFABRIC_HYBRID_EXTEND_LIB_PATH");
    if (path == nullptr) {
        SM_LOG_WARN("Environment MEMFABRIC_HYBRID_EXTEND_LIB_PATH is not set.");
        return false;
    }
    std::string libPath = std::string(path);
    if (access(libPath.c_str(), F_OK) != 0) {
        SM_LOG_WARN("Environment MEMFABRIC_HYBRID_EXTEND_LIB_PATH check failed: " << libPath);
        return false;
    }

    std::string realPath = libPath + "/" + SMEM_RALLOC_DEVICE_LIB_NAME;
    if (access(realPath.c_str(), F_OK) != 0) {
        SM_LOG_WARN("device rdma kernel library not found: " << realPath);
        return false;
    }

    libHandle = dlopen(realPath.c_str(), RTLD_NOW | RTLD_NODELETE);
    if (libHandle == nullptr) {
        SM_LOG_WARN("Failed to open library [" << realPath << "], error: " << dlerror());
        return false;
    }

    pWriteRunSubmit = reinterpret_cast<SmemRallocDeviceWriteRunFunc>(
        dlsym(libHandle, "smem_ralloc_device_write_run_submit"));
    pReadRunSubmit = reinterpret_cast<SmemRallocDeviceReadRunFunc>(
        dlsym(libHandle, "smem_ralloc_device_read_run_submit"));
    pBatchRunSubmit = reinterpret_cast<SmemRallocDeviceBatchRunFunc>(
        dlsym(libHandle, "smem_ralloc_device_batch_run_submit"));
    pDumpRunSubmit = reinterpret_cast<SmemRallocDeviceDumpRunFunc>(
        dlsym(libHandle, "smem_ralloc_device_dump_run_submit"));
    if (pWriteRunSubmit == nullptr || pReadRunSubmit == nullptr || pBatchRunSubmit == nullptr) {
        SM_LOG_WARN("Failed to load symbol smem_ralloc_device_run_submit, error: " << dlerror());
        dlclose(libHandle);
        libHandle = nullptr;
        return false;
    }
    /* dump entry is bring-up-only: a stale library without it stays usable for data copies */

    gLoaded = true;
    return true;
}

void DlSmemRallocDeviceApi::CleanupLibrary()
{
    std::lock_guard<std::mutex> guard(gMutex);
    if (!gLoaded) {
        return;
    }

    pWriteRunSubmit = nullptr;
    pReadRunSubmit = nullptr;
    pBatchRunSubmit = nullptr;
    pDumpRunSubmit = nullptr;
    if (libHandle != nullptr) {
        dlclose(libHandle);
        libHandle = nullptr;
    }
    gLoaded = false;
}
} // namespace smem
} // namespace ock
