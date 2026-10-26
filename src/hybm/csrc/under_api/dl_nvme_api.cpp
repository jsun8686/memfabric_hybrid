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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <dirent.h>
#include <dlfcn.h>
#include <sched.h>
#include "hybm_define.h"

namespace ock {
namespace mf {

bool DlNvmeApi::gLoaded = false;
bool DlNvmeApi::gSpdkEnvReady = false;
SpdkMemRegisterFunc DlNvmeApi::gSpdkMemRegister = nullptr;
SpdkMemUnregisterFunc DlNvmeApi::gSpdkMemUnregister = nullptr;
std::mutex DlNvmeApi::gMutex;
void *DlNvmeApi::gEnvHandle = nullptr;
void *DlNvmeApi::gNvmeHandle = nullptr;

SpdkEnvDpdkPostInitFunc DlNvmeApi::gSpdkEnvDpdkPostInit;
SpdkDmaMallocFunc DlNvmeApi::gSpdkDmaMalloc;
SpdkDmaFreeFunc DlNvmeApi::gSpdkDmaFree;
SpdkNvmeConnectFunc DlNvmeApi::gSpdkNvmeConnect;
SpdkNvmeGetDefaultCtrlrOptsFunc DlNvmeApi::gSpdkNvmeGetDefaultCtrlrOpts;
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

/* try each soname variant of one dependency, first hit wins; handles are leaked
 * on purpose: process-wide lifetime; when every variant fails the last dlerror is
 * surfaced as a WARN -- silent tolerance here cost three debugging rounds once */
static void PreloadFirstOf(const char *const *candidates, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (dlopen(candidates[i], RTLD_NOW | RTLD_GLOBAL) != nullptr) {
            return;
        }
    }
    /* not fatal: the host process may already carry the symbols */
    const char *err = dlerror();
    if (err != nullptr) {
        BM_LOG_WARN("preload of " << candidates[0] << " failed (harmless when the host provides the symbols): " << err);
    } else {
        BM_LOG_WARN("preload of " << candidates[0] << " failed");
    }
}

/* dlopen() every librte_*.so* found in dir (symlink variants resolve to the same
 * object, the refcount bump is idempotent); returns how many were loaded */
static int PreloadDpdkDir(const char *dir)
{
    DIR *dirHandle = opendir(dir);
    if (dirHandle == nullptr) {
        return 0;
    }
    int loaded = 0;
    struct dirent *entry = nullptr;
    while ((entry = readdir(dirHandle)) != nullptr) {
        if (strncmp(entry->d_name, "librte_", strlen("librte_")) != 0 || strstr(entry->d_name, ".so") == nullptr) {
            continue;
        }
        std::string path(dir);
        path += "/";
        path += entry->d_name;
        if (dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL) != nullptr) {
            loaded++;
        }
    }
    closedir(dirHandle);
    return loaded;
}

/* SPDK shared objects omit several runtime deps from DT_NEEDED (native apps such
 * as nvmf_tgt get them at link time); a dlopen host must inject them into the
 * global namespace itself, or RTLD_NOW fails with "undefined symbol". Verified
 * dependency chain: libcrypto (EVP_MD_CTX_free) -> libisal (crc16_t10dif) ->
 * DPDK set (rte_mempool_ops_table, ...) -> libssl (OPENSSL_init_ssl) */
static void PreloadDeps()
{
    static const char *cryptoSo[] = {"libcrypto.so.3", "libcrypto.so.1.1", "libcrypto.so"};
    static const char *isalSo[] = {"libisal.so.2", "libisal.so"};
    static const char *sslSo[] = {"libssl.so.3", "libssl.so.1.1", "libssl.so"};
    PreloadFirstOf(cryptoSo, sizeof(cryptoSo) / sizeof(cryptoSo[0]));
    PreloadFirstOf(isalSo, sizeof(isalSo) / sizeof(isalSo[0]));
    PreloadFirstOf(sslSo, sizeof(sslSo) / sizeof(sslSo[0]));

    /* DPDK libraries: an explicit dir wins, otherwise scan every LD_LIBRARY_PATH
     * entry (the deployment already puts <spdk>/dpdk/build/lib there) */
    int dpdkLoaded = 0;
    const char *dpdkDir = getenv("MF_NVME_DPDK_LIB_DIR");
    if (dpdkDir != nullptr && dpdkDir[0] != '\0') {
        dpdkLoaded = PreloadDpdkDir(dpdkDir);
    } else {
        const char *libPath = getenv("LD_LIBRARY_PATH");
        if (libPath != nullptr) {
            std::string dirs(libPath);
            size_t pos = 0;
            while (pos != std::string::npos) {
                size_t colon = dirs.find(':', pos);
                std::string dir = dirs.substr(pos, (colon == std::string::npos) ? std::string::npos : colon - pos);
                if (!dir.empty()) {
                    dpdkLoaded += PreloadDpdkDir(dir.c_str());
                }
                pos = (colon == std::string::npos) ? std::string::npos : colon + 1;
            }
        }
    }
    BM_LOG_DEBUG("nvme dependency preload done, dpdk libraries loaded: " << dpdkLoaded);
}

Result DlNvmeApi::LoadLibrary()
{
    std::lock_guard<std::mutex> guard(gMutex);
    if (gLoaded) {
        return BM_OK;
    }

    PreloadDeps();

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
    /* optional (B3 zero-copy): dynamic memory registration; absent -> zero-copy is
     * globally disabled and every copy rides the staging path */
    DL_LOAD_SYM_OPTIONAL(gSpdkMemRegister, SpdkMemRegisterFunc, gEnvHandle, "spdk_mem_register");
    DL_LOAD_SYM_OPTIONAL(gSpdkMemUnregister, SpdkMemUnregisterFunc, gEnvHandle, "spdk_mem_unregister");
    DL_LOAD_SYM(gSpdkNvmeConnect, SpdkNvmeConnectFunc, gNvmeHandle, "spdk_nvme_connect");
    /* optional: fills the ctrlr opts prefix with library defaults; absent builds
     * fall back to a zero-initialized prefix (only keepAliveTimeoutMs=0 matters) */
    DL_LOAD_SYM_OPTIONAL(gSpdkNvmeGetDefaultCtrlrOpts, SpdkNvmeGetDefaultCtrlrOptsFunc, gNvmeHandle,
                         "spdk_nvme_ctrlr_get_default_ctrlr_opts");
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

    /* the sock implementations register themselves (and set the default impl) via
     * SPDK_NET_IMPL_REGISTER_DEFAULT constructors that only run when the library is
     * loaded -- apps pull them in at link time, a dlopen host must load them or
     * spdk_sock_connect_ext silently returns NULL without ever calling connect().
     * ORDER MATTERS and is verified the hard way: libspdk_util references env-layer
     * symbols (spdk_realloc) whose provider libspdk_env_dpdk.so is NOT recorded in
     * its DT_NEEDED -- loading the sock impl before the env library fails with
     * "undefined symbol: spdk_realloc", so this must run after the loads above */
    static const char *sockSo[] = {"libspdk_sock_posix.so"};
    PreloadFirstOf(sockSo, sizeof(sockSo) / sizeof(sockSo[0]));
    return BM_OK;
}

Result DlNvmeApi::InitDpdkEnv()
{
    /* spdk_dma_malloc and the qpair socket buffers need a live DPDK EAL; the
     * post-init path assumes the host brought it up itself, so do it here:
     * --in-memory avoids hugepage and shared-config-file dependencies entirely,
     * --no-pci skips ALL pci probing (we only use the memory subsystem -- the
     * NVMe-oF fabric rides rdma-core verbs, not a DPDK PMD): without it EAL
     * tries to bind every device and FAILS on a node whose RDMA nics are taken
     * by another DPDK process (verified: nvmf tgt + daemon co-located) */
    static bool attempted = false;
    if (attempted) {
        return BM_OK;
    }
    attempted = true;

    using RteEalInitFunc = int (*)(int, char **);
    auto rteEalInit = reinterpret_cast<RteEalInitFunc>(dlsym(RTLD_DEFAULT, "rte_eal_init"));
    if (rteEalInit == nullptr) {
        return BM_NOT_SUPPORTED; /* no DPDK mapped in: a pre-inited host, or UT stubs */
    }

    /* static storage: EAL keeps pointers into argv for the process lifetime */
    static char arg0[] = "memfabric-nvme";
    static char arg1[] = "--in-memory";
    static char arg2[] = "--no-pci";
    static char lcoreArg[32] = {0};
    static char iovaArg[32] = {0};

    /* IOVA mode: default VA. In PA mode spdk_mem_register breaks on the vtophys
     * mem map -- rte_mem_virt2iova falls back to /proc/self/pagemap and the
     * translation must be PHYSICALLY 2MB-aligned per chunk (vtophys_notify:
     * "paddr & MASK_2MB -> -EINVAL"; PFN unavailable -> -EFAULT), so ordinary
     * 4K-page user buffers can never register (verified on the 33/35 cluster:
     * zc register failed with zero spdk log output -- vtophys failures are
     * DEBUG_PRINT-only in release builds). With IOVA=VA virt2iova returns the
     * VA itself: the 2MB-aligned inner window passes, and the vtophys entries
     * holding VAs are harmless -- NVMe-oF SGLs address memory via the MR lkey
     * and never consume vtophys (that is the local PCIe PRP path). No PCI
     * devices are bound (--no-pci), so EAL accepts VA without DMA-mask checks.
     * MF_NVME_IOVA_MODE=auto omits the flag (DPDK auto-selects) as an escape
     * hatch for exotic environments that refuse VA. */
    {
        const char *mode = getenv("MF_NVME_IOVA_MODE");
        if (mode == nullptr || mode[0] == '\0') {
            (void)snprintf(iovaArg, sizeof(iovaArg), "%s", "--iova-mode=va");
        } else if (strcmp(mode, "auto") != 0) {
            (void)snprintf(iovaArg, sizeof(iovaArg), "--iova-mode=%s", mode); /* va / pa */
        } /* "auto": leave empty = omit, DPDK picks */
    }

    /* RTE_MAX_LCORE is 128 in this DPDK build: EAL derives its main lcore from the
     * process affinity, and a process pinned to HIGH physical cpus (e.g. NUMA node 6
     * = cpus 144+, the NIC-local pinning) breaks that auto-selection with a misleading
     * "Invalid 'command line' arguments" -- map lcore 0 onto the first allowed cpu
     * explicitly. Only the high-cpu case needs the argument; the already verified
     * unpinned / low-core paths keep their exact argv */
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    if (sched_getaffinity(0, sizeof(affinity), &affinity) == 0) {
        int firstCpu = -1;
        for (int c = 0; c < CPU_SETSIZE; ++c) {
            if (CPU_ISSET(c, &affinity) != 0) {
                firstCpu = c;
                break;
            }
        }
        if (firstCpu >= 128) {
            (void)snprintf(lcoreArg, sizeof(lcoreArg), "--lcores=0@%d", firstCpu);
        }
    }

    char *argv[7] = {arg0, arg1, arg2, nullptr, nullptr, nullptr, nullptr};
    int argc = 3;
    if (lcoreArg[0] != '\0') {
        argv[argc++] = lcoreArg;
    }
    if (iovaArg[0] != '\0') {
        argv[argc++] = iovaArg;
    }
    int rc = rteEalInit(argc, argv);
    if (rc < 0) {
        /* a negative return is only benign when the EAL is already up (another
         * component initialized it) -- anything else is a real failure that must
         * not be papered over: a half-dead EAL silently breaks spdk env
         * post-init (mem_map subsystem), which later surfaces as the cryptic
         * "Unable to create memory map" on the first fabric connect */
        auto *rteErrno = reinterpret_cast<int *>(dlsym(RTLD_DEFAULT, "rte_errno"));
        const int err = rteErrno != nullptr ? *rteErrno : 0;
        if (err == EALREADY) {
            BM_LOG_DEBUG("dpdk eal already initialized by another component, rc:" << rc);
            return BM_OK;
        }
        BM_LOG_ERROR("rte_eal_init failed, rc:" << rc << " rte_errno:" << err
                                                << " -- spdk dma memory will not work");
        return BM_ERROR;
    }
    BM_LOG_DEBUG("rte_eal_init --in-memory --no-pci rc:" << rc);
    return BM_OK;
}

Result DlNvmeApi::EnsureSpdkEnv()
{
    /* process-wide once-per-success: dlopen + EAL + post_init. post_init is NOT
     * re-entrant (DPDK rejects the second same-name mem event callback with EEXIST,
     * which fails mem_map_init), so it must never run again after the first success,
     * even when an nvme data operator is destroyed and re-created with the process
     * still alive. A failure leaves the latch open: fix the env and the next call
     * retries. gEnvMutex nests strictly outside gMutex (LoadLibrary takes gMutex),
     * the order is fixed so there is no deadlock. */
    static std::mutex envMutex;
    std::lock_guard<std::mutex> guard(envMutex);
    if (gSpdkEnvReady) {
        return BM_OK;
    }

    auto ret = LoadLibrary();
    if (ret != BM_OK) {
        return ret;
    }
    ret = InitDpdkEnv();
    if (ret != BM_OK && ret != BM_NOT_SUPPORTED) {
        return ret; /* real EAL failure (e.g. pci clash): hard error, retry stays possible */
    }
    if (gSpdkEnvDpdkPostInit != nullptr) {
        /* the symbol exists: its failure leaves no mem_map subsystem and every RDMA
         * connect after it fails -- report it right here, do not defer the surprise */
        const int postRet = gSpdkEnvDpdkPostInit(false);
        if (postRet != 0) {
            BM_LOG_ERROR("spdk_env_dpdk_post_init failed, ret:" << postRet);
            return BM_ERROR;
        }
    }
    gSpdkEnvReady = true;
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
    gSpdkNvmeGetDefaultCtrlrOpts = nullptr;
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
