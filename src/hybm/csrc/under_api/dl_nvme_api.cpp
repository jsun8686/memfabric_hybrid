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
#include "dl_nvme_api.h"
#include <cstdlib>
#include <dlfcn.h>
#include "hybm_define.h"

namespace ock {
namespace mf {

bool DlNvmeApi::gLoaded = false;
std::mutex DlNvmeApi::gMutex;
void *DlNvmeApi::gEnvHandle = nullptr;
void *DlNvmeApi::gNvmeHandle = nullptr;

SpdkEnvDpdkPostInitFunc DlNvmeApi::gSpdkEnvDpdkPostInit;
SpdkDmaMallocFunc DlNvmeApi::gSpdkDmaMalloc;
SpdkDmaFreeFunc DlNvmeApi::gSpdkDmaFree;
SpdkNvmeConnectFunc DlNvmeApi::gSpdkNvmeConnect;
SpdkNvmeDetachFunc DlNvmeApi::gSpdkNvmeDetach;
SpdkNvmeCtrlrGetNsFunc DlNvmeApi::gSpdkNvmeCtrlrGetNs;
SpdkNvmeCtrlrAllocIoQpairFunc DlNvmeApi::gSpdkNvmeCtrlrAllocIoQpair;
SpdkNvmeCtrlrFreeIoQpairFunc DlNvmeApi::gSpdkNvmeCtrlrFreeIoQpair;
SpdkNvmeCtrlrGetMaxXferSizeFunc DlNvmeApi::gSpdkNvmeCtrlrGetMaxXferSize;
SpdkNvmeNsCmdReadFunc DlNvmeApi::gSpdkNvmeNsCmdRead;
SpdkNvmeNsCmdWriteFunc DlNvmeApi::gSpdkNvmeNsCmdWrite;
SpdkNvmeQpairProcessCompletionsFunc DlNvmeApi::gSpdkNvmeQpairProcessCompletions;

static const char *EnvLibName()
{
    const char *env = getenv("MF_NVME_SPDK_ENV_LIB");
    return (env != nullptr && env[0] != '\0') ? env : "libspdk_env_dpdk.so";
}

static const char *NvmeLibName()
{
    const char *env = getenv("MF_NVME_SPDK_NVME_LIB");
    return (env != nullptr && env[0] != '\0') ? env : "libspdk_nvme.so";
}

Result DlNvmeApi::LoadLibrary()
{
    std::lock_guard<std::mutex> guard(gMutex);
    if (gLoaded) {
        return BM_OK;
    }

    /* the env library exports the DMA allocator, load it GLOBAL so the nvme library
     * resolves its env dependency through the same mapping */
    gEnvHandle = dlopen(EnvLibName(), RTLD_NOW | RTLD_GLOBAL | RTLD_NODELETE);
    if (gEnvHandle == nullptr) {
        BM_LOG_ERROR("failed to open SPDK env library [" << EnvLibName() << "], set LD_LIBRARY_PATH or "
                                                         << "MF_NVME_SPDK_ENV_LIB, error: " << dlerror());
        return BM_DL_FUNCTION_FAILED;
    }

    gNvmeHandle = dlopen(NvmeLibName(), RTLD_NOW | RTLD_NODELETE);
    if (gNvmeHandle == nullptr) {
        BM_LOG_ERROR("failed to open SPDK nvme library [" << NvmeLibName() << "], set LD_LIBRARY_PATH or "
                                                          << "MF_NVME_SPDK_NVME_LIB, error: " << dlerror());
        dlclose(gEnvHandle);
        gEnvHandle = nullptr;
        return BM_DL_FUNCTION_FAILED;
    }

    /* optional: when the process does not run its own SPDK env (dpdk_eal pre-init), this
     * switches the env to dynamic memory registration mode; absent/failed is tolerated */
    DL_LOAD_SYM_OPTIONAL(gSpdkEnvDpdkPostInit, SpdkEnvDpdkPostInitFunc, gEnvHandle, "spdk_env_dpdk_post_init");

    DL_LOAD_SYM(gSpdkDmaMalloc, SpdkDmaMallocFunc, gEnvHandle, "spdk_dma_malloc");
    DL_LOAD_SYM(gSpdkDmaFree, SpdkDmaFreeFunc, gEnvHandle, "spdk_dma_free");
    DL_LOAD_SYM(gSpdkNvmeConnect, SpdkNvmeConnectFunc, gNvmeHandle, "spdk_nvme_connect");
    DL_LOAD_SYM(gSpdkNvmeDetach, SpdkNvmeDetachFunc, gNvmeHandle, "spdk_nvme_detach");
    DL_LOAD_SYM(gSpdkNvmeCtrlrGetNs, SpdkNvmeCtrlrGetNsFunc, gNvmeHandle, "spdk_nvme_ctrlr_get_ns");
    DL_LOAD_SYM(gSpdkNvmeCtrlrAllocIoQpair, SpdkNvmeCtrlrAllocIoQpairFunc, gNvmeHandle,
                "spdk_nvme_ctrlr_alloc_io_qpair");
    DL_LOAD_SYM(gSpdkNvmeCtrlrFreeIoQpair, SpdkNvmeCtrlrFreeIoQpairFunc, gNvmeHandle,
                "spdk_nvme_ctrlr_free_io_qpair");
    /* optional: absent on exotic builds, the operator then clamps to the 128K default */
    DL_LOAD_SYM_OPTIONAL(gSpdkNvmeCtrlrGetMaxXferSize, SpdkNvmeCtrlrGetMaxXferSizeFunc, gNvmeHandle,
                         "spdk_nvme_ctrlr_get_max_xfer_size");
    DL_LOAD_SYM(gSpdkNvmeNsCmdRead, SpdkNvmeNsCmdReadFunc, gNvmeHandle, "spdk_nvme_ns_cmd_read");
    DL_LOAD_SYM(gSpdkNvmeNsCmdWrite, SpdkNvmeNsCmdWriteFunc, gNvmeHandle, "spdk_nvme_ns_cmd_write");
    DL_LOAD_SYM(gSpdkNvmeQpairProcessCompletions, SpdkNvmeQpairProcessCompletionsFunc, gNvmeHandle,
                "spdk_nvme_qpair_process_completions");

    gLoaded = true;
    BM_LOG_INFO("load SPDK env/nvme libraries for DlNvmeApi success");
    return BM_OK;
}

void DlNvmeApi::CleanupLibrary()
{
    std::lock_guard<std::mutex> guard(gMutex);
    if (!gLoaded) {
        return;
    }

    gSpdkEnvDpdkPostInit = nullptr;
    gSpdkDmaMalloc = nullptr;
    gSpdkDmaFree = nullptr;
    gSpdkNvmeConnect = nullptr;
    gSpdkNvmeDetach = nullptr;
    gSpdkNvmeCtrlrGetNs = nullptr;
    gSpdkNvmeCtrlrAllocIoQpair = nullptr;
    gSpdkNvmeCtrlrFreeIoQpair = nullptr;
    gSpdkNvmeCtrlrGetMaxXferSize = nullptr;
    gSpdkNvmeNsCmdRead = nullptr;
    gSpdkNvmeNsCmdWrite = nullptr;
    gSpdkNvmeQpairProcessCompletions = nullptr;

    dlclose(gNvmeHandle);
    gNvmeHandle = nullptr;
    dlclose(gEnvHandle);
    gEnvHandle = nullptr;
    gLoaded = false;
}

} // namespace mf
} // namespace ock
