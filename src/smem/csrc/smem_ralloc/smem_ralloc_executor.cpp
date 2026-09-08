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
#include "smem_ralloc_executor.h"

#include "smem_logger.h"
#include "smem_ralloc_entry.h"
#include "smem_ralloc_entry_manager.h"
#include "smem_ralloc_helper.h"
#include "smem_ralloc_rpc.h"

namespace ock {
namespace smem {
SmemRallocExecutor &SmemRallocExecutor::Instance()
{
    static SmemRallocExecutor instance;
    return instance;
}

Result SmemRallocExecutor::Start()
{
    SmemRallocRpcService::Instance().RegisterHandler(
        SMEMRA_RPC_OP_JOIN_ALLOC, [](SmemRallocRpcMsg &m) { return OnJoinAlloc(m); });
    SM_LOG_INFO("ralloc executor started");
    return SM_OK;
}

void SmemRallocExecutor::Stop() {}

Result SmemRallocExecutor::OnJoinAlloc(SmemRallocRpcMsg &msg)
{
    SM_VALIDATE_RETURN(msg.size != 0, "join alloc size is 0", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(msg.size % SMEM_RALLOC_SIZE_ALIGNMENT == 0, "join alloc size is not 2M aligned",
                       SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(msg.maxDramSize != 0, "join alloc maxDramSize is 0", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(msg.size <= msg.maxDramSize, "join alloc size exceeds maxDramSize", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(msg.size <= SMEM_LOCAL_DRAM_SIZE_MAX, "join alloc size exceeds local dram max",
                       SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(msg.dataOpType != 0, "join alloc dataOpType is 0", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(msg.memType == static_cast<uint32_t>(SMEM_RALLOC_MEM_TYPE_HOST),
                       "join alloc mem type is not HOST", SM_NOT_SUPPORTED);

    auto &manager = SmemRallocEntryManager::Instance();
    if (manager.GetConfig().role != SMEM_RALLOC_ROLE_FAR) {
        SM_LOG_ERROR("join alloc rejected on NEAR role node, pool: " << msg.poolId);
        return SM_NOT_SUPPORTED;
    }

    /* extend branch: pool already exists on this node, extend one more block on its local slot */
    SmemRallocEntryPtr existEntry;
    if (manager.GetEntryById(msg.poolId, existEntry) == SM_OK && existEntry != nullptr) {
        smem_ralloc_mem_info_t info{};
        auto extRet = existEntry->ExtendLocalMem(SMEM_RALLOC_MEM_TYPE_HOST, msg.size, &info);
        if (extRet != SM_OK) {
            SM_LOG_ERROR("join alloc extend failed, pool: " << msg.poolId << " ret: " << extRet);
            return extRet;
        }
        /* invariant: reply below is sent strictly after ExtendLocalMem returned. Its internal
         * GroupUpdate returns only after every joined member (including the requester) has
         * imported and mmapped the new block, so the requester can use the replied gva at once */
        msg.gva = reinterpret_cast<uint64_t>(info.gva);
        msg.ownerRank = manager.GetRankId();
        SM_LOG_INFO("join alloc extend success, pool: " << msg.poolId << " requester: " << msg.reqRank
                                                        << " size: " << msg.size << " gva: " << info.gva);
        return SM_OK;
    }

    /* create branch: first JOIN_ALLOC for this pool on this node, build and join */
    SmemRallocEntryPtr entry;
    auto ret = manager.CreateEntryById(msg.poolId, entry);
    if (ret != SM_OK || entry == nullptr) {
        SM_LOG_ERROR("join alloc create entry(" << msg.poolId << ") failed: " << ret);
        return ret != SM_OK ? ret : SM_ERROR;
    }

    hybm_options options{};
    options.bmType = HYBM_TYPE_HOST_INITIATE;
    options.memType = HYBM_MEM_TYPE_HOST;
    options.bmDataOpType = SmemRallocHelper::TransHybmDataOpType(
        static_cast<smem_ralloc_data_op_type>(msg.dataOpType));
#if !defined(ASCEND_NPU)
    if ((options.bmDataOpType & HYBM_DOP_TYPE_SDMA) || (options.bmDataOpType & HYBM_DOP_TYPE_DEVICE_RDMA)) {
        SM_LOG_ERROR("join alloc entry(" << msg.poolId << ") failed, invalid opType " << options.bmDataOpType
                                         << " for non-cann based backend");
        return SM_ERROR;
    }
#endif
    options.rankCount = manager.GetWorldSize();
    options.rankId = manager.GetRankId();
    options.devId = manager.GetDeviceId();
    options.maxHBMSize = 0;
    options.maxDRAMSize = msg.maxDramSize;
    options.deviceVASpace = 0;
    options.hostVASpace = msg.size; /* X commits the initial block size */
    options.role = HYBM_ROLE_PEER;
    options.flags = msg.flags;
    options.enable56BitsGva = msg.enable56BitsGva;
    bzero(options.transUrl, sizeof(options.transUrl));
    bzero(options.tag, sizeof(options.tag));
    bzero(options.tagOpInfo, sizeof(options.tagOpInfo));
    SmemRallocHelper::TransHybmTlsOption(manager.GetHcomTlsOption(), options.tlsOption);
    if (manager.GetHcomUrl().size() > 64u) {
        SM_LOG_ERROR("url size is " << manager.GetHcomUrl().size());
        return SM_INVALID_PARAM;
    }
    (void)std::copy_n(manager.GetHcomUrl().c_str(), manager.GetHcomUrl().size(), options.transUrl);
    options.scene = HYBM_SCENE_DEFAULT;
    options.dramShmFd = -1;

    ret = entry->Initialize(options);
    if (ret != SM_OK) {
        SM_LOG_ERROR("join alloc entry init failed, result: " << ret);
        (void)manager.RemoveEntryByPtr(reinterpret_cast<uintptr_t>(entry.Get()));
        return ret;
    }

    /* invariant: reply below is sent strictly after Join returned. The GroupJoin barrier
     * guarantees every existing member (including the requester) has imported this node's
     * entity before the reply leaves this node */
    ret = entry->Join(0);
    if (ret != SM_OK) {
        SM_LOG_ERROR("join alloc entry join failed, result: " << ret);
        entry->UnInitialize();
        (void)manager.RemoveEntryByPtr(reinterpret_cast<uintptr_t>(entry.Get()));
        return ret;
    }

    /* gva of the initial block = own slot base (first slice va), NOT the window base */
    smem_ralloc_mem_info_t info{};
    ret = entry->GetLocalMemInfo(&info);
    if (ret != SM_OK || info.gva == nullptr) {
        SM_LOG_ERROR("join alloc get local mem info failed, pool: " << msg.poolId << " ret: " << ret);
        entry->UnInitialize();
        (void)manager.RemoveEntryByPtr(reinterpret_cast<uintptr_t>(entry.Get()));
        return ret != SM_OK ? ret : SM_ERROR;
    }

    msg.gva = reinterpret_cast<uint64_t>(info.gva);
    msg.ownerRank = manager.GetRankId();
    SM_LOG_INFO("join alloc success, pool: " << msg.poolId << " requester: " << msg.reqRank
                                             << " size: " << msg.size << " gva: " << info.gva);
    return SM_OK;
}
} // namespace smem
} // namespace ock
