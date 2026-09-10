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
#include <algorithm>
#include <limits>
#include "smem_common_includes.h"
#include "hybm_big_mem.h"
#include "smem_logger.h"
#include "smem_ralloc_entry_manager.h"
#include "smem_ralloc_entry.h"
#include "smem_ralloc_helper.h"
#include "smem_ralloc_rpc.h"
#include "mf_rwlock.h"
#include "smem_ralloc.h"

using namespace ock::smem;
using namespace ock::mf;
ReadWriteLock g_smemRallocMutex_;
bool g_smemRallocInited = false;

SMEM_API int32_t smem_ralloc_config_init(smem_ralloc_config_t *config)
{
    SM_VALIDATE_RETURN(config != nullptr, "Invalid config", SM_INVALID_PARAM);
    config->initTimeout = SMEM_DEFAUT_WAIT_TIME;
    config->createTimeout = SMEM_DEFAUT_WAIT_TIME;
    config->controlOperationTimeout = SMEM_DEFAUT_WAIT_TIME;
    config->startConfigStoreServer = true;
    config->startConfigStoreOnly = false;
    config->dynamicWorldSize = false;
    config->unifiedAddressSpace = true;
    config->autoRanking = true;
    config->rankId = std::numeric_limits<uint16_t>::max();
    config->flags = 0;
    config->role = SMEM_RALLOC_ROLE_FAR;
    config->rpcPortBase = SMEM_RALLOC_RPC_PORT_BASE_DEFAULT;
    bzero(config->hcomUrl, sizeof(config->hcomUrl));
    bzero(&config->hcomTlsConfig, sizeof(config->hcomTlsConfig));
    bzero(&config->storeTlsConfig, sizeof(config->storeTlsConfig));
    bzero(&config->rpcTlsConfig, sizeof(config->rpcTlsConfig));
    return SM_OK;
}

static int32_t SmemRallocConfigCheck(const smem_ralloc_config_t *config)
{
    SM_VALIDATE_RETURN(config != nullptr, "config is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(config->unifiedAddressSpace == true, "unifiedAddressSpace must be true", SM_INVALID_PARAM);

    SM_VALIDATE_RETURN(config->initTimeout != 0, "initTimeout is zero", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(config->initTimeout <= SMEM_RALLOC_TIMEOUT_MAX, "initTimeout is too large", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(config->createTimeout != 0, "createTimeout is zero", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(config->createTimeout <= SMEM_RALLOC_TIMEOUT_MAX, "createTimeout is too large", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(config->controlOperationTimeout != 0, "controlOperationTimeout is zero", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(config->controlOperationTimeout <= SMEM_RALLOC_TIMEOUT_MAX,
                       "controlOperationTimeout is too large", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(config->role >= SMEM_RALLOC_ROLE_NEAR && config->role < SMEM_RALLOC_ROLE_BUTT,
                       "role is invalid", SM_INVALID_PARAM);

    // config->rank 在SmemRallocEntryManager::PrepareStore中check
    return 0;
}

SMEM_API int32_t smem_ralloc_init(const char *storeURL, uint32_t worldSize, uint16_t deviceId,
                                  const smem_ralloc_config_t *config)
{
    SM_VALIDATE_RETURN(worldSize != 0, "invalid param, worldSize is 0", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(worldSize <= SMEM_WORLD_SIZE_MAX, "invalid param, worldSize is too large", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(storeURL != nullptr, "invalid param, storeURL is null", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(SmemRallocConfigCheck(config) == 0, "config is invalid", SM_INVALID_PARAM);

    WriteGuard locker(g_smemRallocMutex_);
    if (g_smemRallocInited) {
        SM_LOG_INFO("smem ralloc initialized already");
        return SM_OK;
    }

    int32_t ret = SmemRallocEntryManager::Instance().Initialize(storeURL, worldSize, deviceId, *config);
    if (ret != 0) {
        SM_LOG_AND_SET_LAST_ERROR("init ralloc entry manager failed, result: " << ret);
        return SM_ERROR;
    }

    ret = hybm_init(deviceId, config->flags);
    if (ret != 0) {
        SM_LOG_AND_SET_LAST_ERROR("init hybm failed, result: " << ret << ", flags: 0x" << std::hex << config->flags);
        SmemRallocEntryManager::Instance().Destroy();
        return SM_ERROR;
    }

    g_smemRallocInited = true;
    SM_LOG_INFO("smem_ralloc_init success. "
                << " config_ip: " << storeURL << " rank: " << SmemRallocEntryManager::Instance().GetRankId());
    return SM_OK;
}

SMEM_API void smem_ralloc_uninit(uint32_t flags)
{
    WriteGuard locker(g_smemRallocMutex_);
    if (!g_smemRallocInited) {
        SM_LOG_WARN("smem ralloc not initialized yet");
        return;
    }

    // Destroy entries first (may call GroupLeave and hybm_* cleanup) before tearing
    // down the underlying hybm layer. This prevents use-after-free when UnInitialize()
    // accesses entity_ or hybm resources during Destroy().
    SmemRallocEntryManager::Instance().Destroy();
    hybm_uninit();
    g_smemRallocInited = false;
    SM_LOG_INFO("smem_ralloc_uninit finished");
}

SMEM_API uint32_t smem_ralloc_get_rank_id(void)
{
    return SmemRallocEntryManager::Instance().GetRankId();
}

static inline int32_t SmemRallocDataOpCheck(smem_ralloc_data_op_type dataOpType)
{
    constexpr uint32_t dataOpTypeMask = SMEMRA_DATA_OP_SDMA | SMEMRA_DATA_OP_HOST_RDMA | SMEMRA_DATA_OP_HOST_URMA |
                                        SMEMRA_DATA_OP_HOST_TCP | SMEMRA_DATA_OP_DEVICE_RDMA;
    return (dataOpType & dataOpTypeMask) != 0;
}

static inline bool SmemRallocCreateOptionCheck(const smem_ralloc_create_option_t *option)
{
    SM_VALIDATE_RETURN(option != nullptr, "option is null", false);
    SM_VALIDATE_RETURN(!(option->maxDramSize == 0UL && option->maxHbmSize == 0UL), "maxMemorySize is 0", false);
    SM_VALIDATE_RETURN(option->maxDramSize % SMEM_RALLOC_SIZE_ALIGNMENT == 0UL, "maxDramSize is not 2M aligned",
                       false);
    SM_VALIDATE_RETURN(option->maxHbmSize % SMEM_RALLOC_SIZE_ALIGNMENT == 0UL, "maxHbmSize is not 2M aligned",
                       false);
    return true;
}

static int32_t smem_ralloc_create_inner(uint32_t id, const smem_ralloc_create_option_t *option, smem_ralloc_t *out)
{
    *out = nullptr;
    if (!g_smemRallocInited) {
        SM_LOG_AND_SET_LAST_ERROR_CODE(SM_NOT_INITIALIZED, "smem ralloc not initialized yet");
        return SM_NOT_INITIALIZED;
    }
    auto &manager = SmemRallocEntryManager::Instance();
    if (manager.GetConfig().role != SMEM_RALLOC_ROLE_NEAR) {
        SM_LOG_AND_SET_LAST_ERROR_CODE(SM_NOT_SUPPORTED, "smem_ralloc_create is only supported on NEAR role nodes");
        return SM_NOT_SUPPORTED;
    }
    if (!SmemRallocCreateOptionCheck(option)) {
        SM_LOG_AND_SET_LAST_ERROR_CODE(SM_INVALID_PARAM, "create option is invalid");
        return SM_INVALID_PARAM;
    }
    if (SmemRallocDataOpCheck(option->dataOpType) == 0) {
        SM_LOG_AND_SET_LAST_ERROR_CODE(SM_INVALID_PARAM, "invalid data op type: " << option->dataOpType);
        return SM_INVALID_PARAM;
    }
    if ((option->dataOpType & SMEMRA_DATA_OP_HOST_SHM) != 0U) {
        SM_LOG_AND_SET_LAST_ERROR_CODE(SM_INVALID_PARAM, "HOST_SHM op type is not supported by ralloc");
        return SM_INVALID_PARAM;
    }

    SmemRallocEntryPtr entry;
    auto ret = manager.CreateEntryById(id, entry);
    if (ret != 0 || entry == nullptr) {
        SM_LOG_AND_SET_LAST_ERROR_CODE(ret != 0 ? ret : SM_ERROR,
            "create ralloc entity(" << id << ") failed: " << ret);
        return ret != 0 ? ret : SM_ERROR;
    }
    /* from here on the entry is tracked by the manager, every failure path must clean it up */
    auto cleanup = [&manager, &entry]() {
        entry->UnInitialize();
        (void)manager.RemoveEntryByPtr(reinterpret_cast<uintptr_t>(entry.Get()));
    };

    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    options.memType = SmemRallocHelper::TransHybmMemType(option->maxDramSize, option->maxHbmSize);
    options.bmDataOpType = SmemRallocHelper::TransHybmDataOpType(option->dataOpType);
#if !defined(ASCEND_NPU)
    if ((options.bmDataOpType & HYBM_DOP_TYPE_SDMA) || (options.bmDataOpType & HYBM_DOP_TYPE_DEVICE_RDMA)) {
        SM_LOG_AND_SET_LAST_ERROR_CODE(SM_ERROR,
            "create ralloc entity(" << id << ") failed, invalid opType " << options.bmDataOpType
                                    << " for non-cann based backend");
        cleanup();
        return SM_ERROR;
    }
#endif
    options.rankCount = manager.GetWorldSize();
    options.rankId = manager.GetRankId();
    options.devId = manager.GetDeviceId();
    options.maxHBMSize = option->maxHbmSize; /* >0 reserves the HBM window, commit still comes from extend_* */
    options.maxDRAMSize = option->maxDramSize;
    options.deviceVASpace = 0;
    options.hostVASpace = 0; /* create never commits local memory, blocks come from extend_* only */
    options.role = HYBM_ROLE_PEER;
    options.flags = option->flags;

    constexpr uint64_t SMEM_56BITS_GVA_REQUIRED_THRESHOLD = 32ULL << 40; // 32TB
    const uint64_t totalAddrSpace =
        (option->maxDramSize + option->maxHbmSize) * static_cast<uint64_t>(options.rankCount);
    if (!option->enable56BitsGva && totalAddrSpace > SMEM_56BITS_GVA_REQUIRED_THRESHOLD) {
        SM_LOG_AND_SET_LAST_ERROR_CODE(SM_INVALID_PARAM,
            "total address space (" << totalAddrSpace << " B) exceeds 32TB but enable56BitsGva is false. "
            << "Please set enable56BitsGva = true, "
            << "maxDram=" << option->maxDramSize << ", maxHbm=" << option->maxHbmSize
            << ", rankCount=" << options.rankCount);
        cleanup();
        return SM_INVALID_PARAM;
    }
    options.enable56BitsGva = option->enable56BitsGva;
    bzero(options.transUrl, sizeof(options.transUrl));
    bzero(options.tag, sizeof(options.tag));
    bzero(options.tagOpInfo, sizeof(options.tagOpInfo));

    SmemRallocHelper::TransHybmTlsOption(manager.GetHcomTlsOption(), options.tlsOption);

    if (manager.GetHcomUrl().size() > 64u) {
        SM_LOG_AND_SET_LAST_ERROR_CODE(SM_INVALID_PARAM,
            "url size is " << manager.GetHcomUrl().size());
        cleanup();
        return SM_INVALID_PARAM;
    }
    (void)std::copy_n(manager.GetHcomUrl().c_str(), manager.GetHcomUrl().size(), options.transUrl);

    options.scene = HYBM_SCENE_DEFAULT;
    options.dramShmFd = -1;
    ret = entry->Initialize(options);
    if (ret != 0) {
        SM_LOG_AND_SET_LAST_ERROR_CODE(ret, "entry init failed, result: " << ret);
        cleanup();
        return ret;
    }
    ret = entry->Join(0);
    if (ret != 0) {
        SM_LOG_AND_SET_LAST_ERROR_CODE(ret, "entry join failed, result: " << ret);
        cleanup();
        return ret;
    }

    *out = reinterpret_cast<void *>(entry.Get());
    SM_LOG_INFO("ralloc create success, id: " << id << " rank: " << manager.GetRankId());
    return SM_OK;
}

SMEM_API smem_ralloc_t smem_ralloc_create(uint32_t id, const smem_ralloc_create_option_t *option)
{
    smem_ralloc_t handle = nullptr;
    int32_t ret = smem_ralloc_create_inner(id, option, &handle);
    if (ret != SM_OK) {
        SM_LOG_AND_SET_LAST_ERROR_CODE(ret, "smem_ralloc_create failed");
        return nullptr;
    }
    return handle;
}

SMEM_API void smem_ralloc_destroy(smem_ralloc_t handle)
{
    SM_ASSERT_RET_VOID(handle != nullptr);
    SM_ASSERT_RET_VOID(g_smemRallocInited);
    SmemRallocEntryPtr entry = nullptr;
    auto ret = SmemRallocEntryManager::Instance().GetEntryByPtr(reinterpret_cast<uintptr_t>(handle), entry);
    if (ret != SM_OK || entry == nullptr) {
        SM_LOG_WARN("input handle is invalid, result: " << ret);
        return;
    }
    entry->UnInitialize();
    entry = nullptr;
    ret = SmemRallocEntryManager::Instance().RemoveEntryByPtr(reinterpret_cast<uintptr_t>(handle));
    SM_ASSERT_RET_VOID(ret == SM_OK);
}

SMEM_API int32_t smem_ralloc_copy(smem_ralloc_t handle, const void *src, void *dest, uint64_t size, uint32_t flags)
{
    SM_VALIDATE_RETURN(handle != nullptr, "invalid param, handle is NULL", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(g_smemRallocInited, "smem ralloc not initialized yet", SM_NOT_INITIALIZED);

    SmemRallocEntryPtr entry = nullptr;
    auto ret = SmemRallocEntryManager::Instance().GetEntryByPtr(reinterpret_cast<uintptr_t>(handle), entry);
    if (ret != SM_OK || entry == nullptr) {
        SM_LOG_AND_SET_LAST_ERROR("input handle is invalid, result: " << ret);
        return SM_INVALID_PARAM;
    }

    return entry->DataCopy(src, dest, size, flags);
}

SMEM_API int32_t smem_ralloc_wait(smem_ralloc_t handle)
{
    SM_VALIDATE_RETURN(handle != nullptr, "invalid param, handle is NULL", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(g_smemRallocInited, "smem ralloc not initialized yet", SM_NOT_INITIALIZED);

    SmemRallocEntryPtr entry = nullptr;
    auto ret = SmemRallocEntryManager::Instance().GetEntryByPtr(reinterpret_cast<uintptr_t>(handle), entry);
    if (ret != SM_OK || entry == nullptr) {
        SM_LOG_AND_SET_LAST_ERROR("input handle is invalid, result: " << ret);
        return SM_INVALID_PARAM;
    }

    return entry->Wait();
}

SMEM_API int32_t smem_ralloc_register_user_mem(smem_ralloc_t handle, uint64_t addr, uint64_t size)
{
    SM_VALIDATE_RETURN(handle != nullptr, "invalid param, handle is NULL", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(addr != 0, "invalid param, addr eq 0", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(g_smemRallocInited, "smem ralloc not initialized yet", SM_NOT_INITIALIZED);

    SmemRallocEntryPtr entry = nullptr;
    auto ret = SmemRallocEntryManager::Instance().GetEntryByPtr(reinterpret_cast<uintptr_t>(handle), entry);
    if (ret != SM_OK || entry == nullptr) {
        SM_LOG_AND_SET_LAST_ERROR("input handle is invalid, result: " << ret);
        return SM_INVALID_PARAM;
    }

    return entry->RegisterMem(addr, size);
}

SMEM_API int32_t smem_ralloc_unregister_user_mem(smem_ralloc_t handle, uint64_t addr)
{
    SM_VALIDATE_RETURN(handle != nullptr, "invalid param, handle is NULL", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(addr != 0, "invalid param, addr eq 0", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(g_smemRallocInited, "smem ralloc not initialized yet", SM_NOT_INITIALIZED);

    SmemRallocEntryPtr entry = nullptr;
    auto ret = SmemRallocEntryManager::Instance().GetEntryByPtr(reinterpret_cast<uintptr_t>(handle), entry);
    if (ret != SM_OK || entry == nullptr) {
        SM_LOG_AND_SET_LAST_ERROR("input handle is invalid, result: " << ret);
        return SM_INVALID_PARAM;
    }

    return entry->UnRegisterMem(addr);
}

SMEM_API int32_t smem_ralloc_extend_local_mem(smem_ralloc_t handle, smem_ralloc_mem_type_t memType, uint64_t size,
                                              smem_ralloc_mem_info_t *info)
{
    SM_VALIDATE_RETURN(handle != nullptr, "invalid param, handle is NULL", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(g_smemRallocInited, "smem ralloc not initialized yet", SM_NOT_INITIALIZED);
    SM_VALIDATE_RETURN(size > 0, "invalid param, size is 0", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(size % SMEM_RALLOC_SIZE_ALIGNMENT == 0, "invalid param, size is not 2M aligned",
                       SM_INVALID_PARAM);

    SmemRallocEntryPtr entry = nullptr;
    auto ret = SmemRallocEntryManager::Instance().GetEntryByPtr(reinterpret_cast<uintptr_t>(handle), entry);
    if (ret != SM_OK || entry == nullptr) {
        SM_LOG_AND_SET_LAST_ERROR("input handle is invalid, result: " << ret);
        return SM_INVALID_PARAM;
    }

    return entry->ExtendLocalMem(memType, size, info);
}

SMEM_API int32_t smem_ralloc_extend_remote_mem(smem_ralloc_t handle, smem_ralloc_mem_type_t memType, uint64_t size,
                                               smem_ralloc_mem_info_t *info)
{
    SM_VALIDATE_RETURN(handle != nullptr, "invalid param, handle is NULL", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(g_smemRallocInited, "smem ralloc not initialized yet", SM_NOT_INITIALIZED);
    SM_VALIDATE_RETURN(memType == SMEM_RALLOC_MEM_TYPE_HOST || memType == SMEM_RALLOC_MEM_TYPE_DEVICE,
        "invalid param, mem type must be HOST or DEVICE", SM_NOT_SUPPORTED);
    SM_VALIDATE_RETURN(size > 0, "invalid param, size is 0", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(size % SMEM_RALLOC_SIZE_ALIGNMENT == 0, "invalid param, size is not 2M aligned",
                       SM_INVALID_PARAM);

    if (info != nullptr) {
        info->rankId = SMEM_RALLOC_INVALID_RANK;
        info->gva = nullptr;
    }

    SmemRallocEntryPtr entry = nullptr;
    auto &manager = SmemRallocEntryManager::Instance();
    auto ret = manager.GetEntryByPtr(reinterpret_cast<uintptr_t>(handle), entry);
    if (ret != SM_OK || entry == nullptr) {
        SM_LOG_AND_SET_LAST_ERROR("input handle is invalid, result: " << ret);
        return SM_INVALID_PARAM;
    }

    auto &rpc = SmemRallocRpcService::Instance();
    if (!manager.HasMaster()) {
        SM_LOG_AND_SET_LAST_ERROR_CODE(SM_NOT_STARTED, "no master in this deployment, remote extend disabled");
        return SM_NOT_STARTED;
    }

    /* 1. ask master to pick the contributor node (least loaded on the requested media, never the requester itself) */
    SmemRallocRpcMsg placeMsg{};
    placeMsg.op = SMEMRA_RPC_OP_PLACEMENT;
    placeMsg.size = size;
    placeMsg.memType = static_cast<uint32_t>(memType);
    auto masterEp = manager.GetMasterEndpoint();
    ret = rpc.SyncCall(masterEp, placeMsg);
    if (ret == SM_NOT_CONNECTED) {
        /* cached master endpoint may be stale (restart/failover), refresh and retry once */
        manager.RefreshMasterEndpoint();
        ret = rpc.SyncCall(manager.GetMasterEndpoint(), placeMsg);
    }
    if (ret != SM_OK || placeMsg.result != SM_OK) {
        SM_LOG_AND_SET_LAST_ERROR_CODE(ret != SM_OK ? ret : placeMsg.result,
            "placement failed, ret: " << ret << " result: " << placeMsg.result);
        return ret != SM_OK ? ret : placeMsg.result;
    }

    /* 2. ask the contributor to build/extend the pool, join on demand and return block gva */
    SmemRallocRpcEndpoint node{};
    node.rankId = placeMsg.nodeRank;
    node.port = static_cast<uint16_t>(placeMsg.nodePort);
    (void)strncpy(node.ip, placeMsg.nodeIp, sizeof(node.ip) - 1);

    const auto &coreOptions = entry->GetCoreOptions();
    SmemRallocRpcMsg allocMsg{};
    allocMsg.op = SMEMRA_RPC_OP_JOIN_ALLOC;
    allocMsg.poolId = entry->Id();
    allocMsg.size = size;
    allocMsg.maxDramSize = coreOptions.maxDRAMSize;
    allocMsg.maxHbmSize = coreOptions.maxHBMSize;
    allocMsg.dataOpType = SmemRallocHelper::TransSmemDataOpType(coreOptions.bmDataOpType);
    allocMsg.flags = coreOptions.flags;
    allocMsg.enable56BitsGva = coreOptions.enable56BitsGva;
    allocMsg.memType = static_cast<uint32_t>(memType);
    ret = rpc.SyncCall(node, allocMsg);
    if (ret != SM_OK || allocMsg.result != SM_OK || allocMsg.gva == 0) {
        SM_LOG_AND_SET_LAST_ERROR_CODE(ret != SM_OK ? ret : allocMsg.result,
            "remote join alloc failed, ret: " << ret << " result: " << allocMsg.result
                                              << " gva: " << allocMsg.gva);
        if (ret != SM_OK) {
            return ret;
        }
        return allocMsg.result != SM_OK ? static_cast<Result>(allocMsg.result) : static_cast<Result>(SM_ERROR);
    }

    /* 3. deliver the block info, gva is usable at once: the contributor replies strictly after
     * its GroupJoin/GroupUpdate barrier returned, by then this node has imported the block */
    if (info != nullptr) {
        info->rankId = allocMsg.ownerRank;
        info->gva = reinterpret_cast<void *>(allocMsg.gva);
    }
    SM_LOG_INFO("remote block created, pool: " << entry->Id() << " ownerRank: " << allocMsg.ownerRank
                                               << " gva: " << reinterpret_cast<void *>(allocMsg.gva)
                                               << " size: " << size);
    return SM_OK;
}

SMEM_API uint64_t smem_ralloc_get_mem_size_by_rank(smem_ralloc_t handle, uint32_t rank,
                                                   smem_ralloc_mem_type_t memType)
{
    SM_VALIDATE_RETURN(handle != nullptr, "invalid param, handle is NULL", 0);
    SM_VALIDATE_RETURN(g_smemRallocInited, "smem ralloc not initialized yet", 0);

    SmemRallocEntryPtr entry = nullptr;
    auto ret = SmemRallocEntryManager::Instance().GetEntryByPtr(reinterpret_cast<uintptr_t>(handle), entry);
    if (ret != SM_OK || entry == nullptr) {
        SM_LOG_AND_SET_LAST_ERROR("input handle is invalid, result: " << ret);
        return 0;
    }

    return entry->GetMemSizeByRank(rank, memType);
}

SMEM_API void *smem_ralloc_get_mem_ptr_by_rank(smem_ralloc_t handle, uint32_t rank,
                                               smem_ralloc_mem_type_t memType)
{
    SM_VALIDATE_RETURN(handle != nullptr, "invalid param, handle is NULL", nullptr);
    SM_VALIDATE_RETURN(g_smemRallocInited, "smem ralloc not initialized yet", nullptr);

    SmemRallocEntryPtr entry = nullptr;
    auto ret = SmemRallocEntryManager::Instance().GetEntryByPtr(reinterpret_cast<uintptr_t>(handle), entry);
    if (ret != SM_OK || entry == nullptr) {
        SM_LOG_AND_SET_LAST_ERROR("input handle is invalid, result: " << ret);
        return nullptr;
    }

    return entry->GetMemPtrByRank(rank, memType);
}

SMEM_API uint32_t smem_ralloc_get_group_ranks(smem_ralloc_t handle, uint32_t *rankIds, uint32_t maxCount)
{
    SM_VALIDATE_RETURN(handle != nullptr, "invalid param, handle is NULL", UINT32_MAX);
    SM_VALIDATE_RETURN(g_smemRallocInited, "smem ralloc not initialized yet", UINT32_MAX);
    SM_VALIDATE_RETURN(rankIds != nullptr || maxCount == 0U, "invalid param, rankIds is NULL", UINT32_MAX);

    SmemRallocEntryPtr entry = nullptr;
    auto ret = SmemRallocEntryManager::Instance().GetEntryByPtr(reinterpret_cast<uintptr_t>(handle), entry);
    if (ret != SM_OK || entry == nullptr) {
        SM_LOG_AND_SET_LAST_ERROR("input handle is invalid, result: " << ret);
        return UINT32_MAX;
    }

    auto ranks = entry->GetGroupRanks();
    auto count = static_cast<uint32_t>(ranks.size());
    if (rankIds != nullptr && count > 0U) {
        auto copyCount = count < maxCount ? count : maxCount;
        std::copy(ranks.begin(), ranks.begin() + copyCount, rankIds);
    }
    return count;
}

SMEM_API int32_t smem_ralloc_set_group_event_handler(smem_ralloc_t handle, smem_ralloc_group_event_cb cb,
                                                     void *context)
{
    SM_VALIDATE_RETURN(handle != nullptr, "invalid param, handle is NULL", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(g_smemRallocInited, "smem ralloc not initialized yet", SM_NOT_INITIALIZED);

    SmemRallocEntryPtr entry = nullptr;
    auto ret = SmemRallocEntryManager::Instance().GetEntryByPtr(reinterpret_cast<uintptr_t>(handle), entry);
    if (ret != SM_OK || entry == nullptr) {
        SM_LOG_AND_SET_LAST_ERROR("input handle is invalid, result: " << ret);
        return SM_INVALID_PARAM;
    }

    return entry->SetGroupEventHandler(cb, context);
}
