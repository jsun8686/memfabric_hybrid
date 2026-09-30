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
#include <chrono>
#include <mutex>
#include "smem_ralloc_executor.h"

#include "smem_logger.h"
#include "mf_env_util.h"
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
    const bool deviceMedia = msg.memType == static_cast<uint32_t>(SMEM_RALLOC_MEM_TYPE_DEVICE);
    if (deviceMedia) {
        SM_VALIDATE_RETURN(msg.maxHbmSize != 0, "join alloc maxHbmSize is 0", SM_INVALID_PARAM);
        SM_VALIDATE_RETURN(msg.size <= msg.maxHbmSize, "join alloc size exceeds maxHbmSize", SM_INVALID_PARAM);
        SM_VALIDATE_RETURN(msg.size <= SMEM_LOCAL_HBM_SIZE_MAX, "join alloc size exceeds local hbm max",
                           SM_INVALID_PARAM);
    } else {
        SM_VALIDATE_RETURN(msg.memType == static_cast<uint32_t>(SMEM_RALLOC_MEM_TYPE_HOST),
                           "join alloc mem type is neither HOST nor DEVICE", SM_NOT_SUPPORTED);
        SM_VALIDATE_RETURN(msg.maxDramSize != 0, "join alloc maxDramSize is 0", SM_INVALID_PARAM);
        SM_VALIDATE_RETURN(msg.size <= msg.maxDramSize, "join alloc size exceeds maxDramSize", SM_INVALID_PARAM);
        SM_VALIDATE_RETURN(msg.size <= SMEM_LOCAL_DRAM_SIZE_MAX, "join alloc size exceeds local dram max",
                           SM_INVALID_PARAM);
    }
    SM_VALIDATE_RETURN(msg.dataOpType != 0, "join alloc dataOpType is 0", SM_INVALID_PARAM);

    auto &manager = SmemRallocEntryManager::Instance();
    if (manager.GetConfig().role != SMEM_RALLOC_ROLE_FAR) {
        SM_LOG_ERROR("join alloc rejected on NEAR role node, pool: " << msg.poolId);
        return SM_NOT_SUPPORTED;
    }

    /* serialize same-pool joins on this node (extend / recycle / create phases). A racing
     * recycle could double-UnInitialize a leftover or slip a second Create past the map
     * dedup, both landing two Initializations (or an UnInitialize against a running
     * Initialize) on the same shared hybm entity slot -- observed as a contributor crash
     * (SIGSEGV) under a 4-client concurrent join. The wait is bounded below the client-side
     * 180 s JOIN_ALLOC rpc timeout so the 2 AccWrk threads can never both park here forever;
     * on timeout the client's bounded re-placement retries cover it. */
    const uint32_t lockWaitSec = mf::MfEnvUtil::GetOptionalUintOrDefault("MF_RALLOC_JOIN_LOCK_WAIT_SEC", 150U);
    /* defer_lock is mandatory: the plain unique_lock(mutex) ctor already calls lock(), and a
     * subsequent try_lock_for would re-enter a mutex this thread owns -- undefined behavior
     * for a non-recursive timed_mutex (observed as the join service stalling: the holder
     * self-waits the full timeout while every other AccWrk parks unbounded in the ctor). */
    std::unique_lock<std::timed_mutex> poolJoinLock(manager.PoolJoinLock(msg.poolId), std::defer_lock);
    if (!poolJoinLock.try_lock_for(std::chrono::seconds(lockWaitSec))) {
        SM_LOG_ERROR("join alloc timed out waiting for the pool join lock, pool: " << msg.poolId
                      << " requester: " << msg.reqRank << " waited: " << lockWaitSec << "s");
        manager.NotifyPlacementFailure(msg);
        return SM_NOT_INITIALIZED;
    }

    /* extend branch: pool already exists on this node, extend one more block on its local slot */
    SmemRallocEntryPtr existEntry;
    if (manager.GetEntryById(msg.poolId, existEntry) == SM_OK && existEntry != nullptr) {
        /* pool-definition consistency: a leftover entry from a previous client (still inside
         * the reap grace window) must not serve a pool with a different layout -- 56-bit GVA
         * mismatch would pair a 64P-window joiner with a legacy-window entity */
        const hybm_options &cur = existEntry->GetCoreOptions();
        auto wantOp = SmemRallocHelper::TransHybmDataOpType(static_cast<smem_ralloc_data_op_type>(msg.dataOpType));
        auto wantBt = ((msg.dataOpType & SMEMRA_DATA_OP_DEVICE_SCHEDULE) != 0U) ? HYBM_TYPE_AI_CORE_INITIATE
                                                                                : HYBM_TYPE_HOST_INITIATE;
        bool mismatch = (cur.maxDRAMSize != msg.maxDramSize) || (cur.maxHBMSize != msg.maxHbmSize) ||
                        (cur.bmDataOpType != wantOp) || (cur.bmType != wantBt) ||
                        (cur.enable56BitsGva != (msg.enable56BitsGva != 0U));
        if (mismatch) {
            if (existEntry->PoolEmpty()) {
                SM_LOG_WARN("join alloc pool-definition mismatch on empty pool: " << msg.poolId
                              << ", recycling the leftover entry (cur dram:" << cur.maxDRAMSize
                              << " hbm:" << cur.maxHBMSize << " op:" << cur.bmDataOpType
                              << " 56bits:" << cur.enable56BitsGva << ", want dram:" << msg.maxDramSize
                              << " hbm:" << msg.maxHbmSize << " op:" << wantOp
                              << " 56bits:" << (msg.enable56BitsGva != 0U) << ")");
                existEntry->UnInitialize();
                (void)manager.RemoveEntryByPtr(reinterpret_cast<uintptr_t>(existEntry.Get()));
                existEntry = nullptr; /* fall through to the create branch with the requested params */
            } else {
                SM_LOG_ERROR("join alloc rejected: pool-definition mismatch on a pool with live members, pool: "
                             << msg.poolId << " (cur dram:" << cur.maxDRAMSize << " hbm:" << cur.maxHBMSize
                             << " op:" << cur.bmDataOpType << " 56bits:" << cur.enable56BitsGva
                             << ", want dram:" << msg.maxDramSize << " hbm:" << msg.maxHbmSize
                             << " op:" << wantOp << " 56bits:" << (msg.enable56BitsGva != 0U) << ")");
                manager.NotifyPlacementFailure(msg);
                return SM_INVALID_PARAM;
            }
        }
    }
    if (manager.GetEntryById(msg.poolId, existEntry) == SM_OK && existEntry != nullptr) {
        smem_ralloc_mem_info_t info{};
        auto extRet = existEntry->ExtendLocalMem(static_cast<smem_ralloc_mem_type_t>(msg.memType), msg.size, &info);
        if (extRet != SM_OK) {
            SM_LOG_ERROR("join alloc extend failed, pool: " << msg.poolId << " ret: " << extRet);
            /* release the master's optimistic reservation: this grant will never land,
             * parking it in the load view until the TTL would skew future placements */
            manager.NotifyPlacementFailure(msg);
            return extRet;
        }
        /* invariant: reply below is sent strictly after ExtendLocalMem returned. Its internal
         * GroupUpdate returns only after every joined member (including the requester) has
         * imported and mmapped the new block, so the requester can use the replied gva at once */
        msg.gva = reinterpret_cast<uint64_t>(info.gva);
        msg.ownerRank = manager.GetRankId();
        SM_LOG_INFO("join alloc extend success, pool: " << msg.poolId << " requester: " << msg.reqRank
                                                        << " size: " << msg.size << " gva: " << info.gva);
        manager.PokeReporter(); /* refresh master LB view without waiting a full period */
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
    if ((msg.dataOpType & SMEMRA_DATA_OP_DEVICE_SCHEDULE) != 0U) {
        if ((msg.dataOpType & SMEMRA_DATA_OP_DEVICE_RDMA) == 0U) {
            SM_LOG_ERROR("join alloc entry(" << msg.poolId << ") failed, DEVICE_SCHEDULE without DEVICE_RDMA");
            return SM_INVALID_PARAM;
        }
        options.bmType = HYBM_TYPE_AI_CORE_INITIATE;
    }
    options.memType = SmemRallocHelper::TransHybmMemType(msg.maxDramSize, msg.maxHbmSize);
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
    options.maxHBMSize = msg.maxHbmSize;
    options.maxDRAMSize = msg.maxDramSize;
    /* X commits the initial block of the requested media, the other window stays reserved-only */
    options.deviceVASpace = deviceMedia ? msg.size : 0;
    options.hostVASpace = deviceMedia ? 0 : msg.size;
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

    /* from here on, concurrent extends park in WaitForBootstrap until this branch
     * finishes, instead of failing fast on the not-yet-inited entry */
    entry->MarkBootstrapRunning();
    ret = entry->Initialize(options);
    if (ret != SM_OK) {
        SM_LOG_ERROR("join alloc entry init failed, result: " << ret);
        entry->MarkBootstrapDone(false);
        manager.NotifyPlacementFailure(msg);
        (void)manager.RemoveEntryByPtr(reinterpret_cast<uintptr_t>(entry.Get()));
        return ret;
    }

    /* invariant: reply below is sent strictly after Join returned. The GroupJoin barrier
     * guarantees every existing member (including the requester) has imported this node's
     * entity before the reply leaves this node */
    ret = entry->Join(0);
    if (ret != SM_OK) {
        SM_LOG_ERROR("join alloc entry join failed, result: " << ret);
        /* wake parked extends before tearing the entry down under them */
        entry->MarkBootstrapDone(false);
        manager.NotifyPlacementFailure(msg);
        entry->UnInitialize();
        (void)manager.RemoveEntryByPtr(reinterpret_cast<uintptr_t>(entry.Get()));
        return ret;
    }

    /* gva of the initial block = own slot base (first slice va), NOT the window base */
    smem_ralloc_mem_info_t info{};
    ret = entry->GetLocalMemInfo(&info);
    if (ret != SM_OK || info.gva == nullptr) {
        SM_LOG_ERROR("join alloc get local mem info failed, pool: " << msg.poolId << " ret: " << ret);
        entry->MarkBootstrapDone(false);
        manager.NotifyPlacementFailure(msg);
        entry->UnInitialize();
        (void)manager.RemoveEntryByPtr(reinterpret_cast<uintptr_t>(entry.Get()));
        return ret != SM_OK ? ret : SM_ERROR;
    }

    msg.gva = reinterpret_cast<uint64_t>(info.gva);
    msg.ownerRank = manager.GetRankId();
    SM_LOG_INFO("join alloc success, pool: " << msg.poolId << " requester: " << msg.reqRank
                                             << " size: " << msg.size << " gva: " << info.gva);
    entry->MarkBootstrapDone(true); /* wake extends parked in WaitForBootstrap */
    manager.PokeReporter(); /* refresh master LB view without waiting a full period */
    return SM_OK;
}
} // namespace smem
} // namespace ock
