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
#include <thread>
#include <algorithm>
#include <cstring>

#include "smem_net_common.h"
#include "smem_net_group_engine.h"
#include "smem_store_factory.h"
#include "smem_tcp_config_store.h"
#include "network_endpoint_util.h"
#include "mf_env_util.h"

#include "smem_ralloc_entry_manager.h"
#include "smem_ralloc_executor.h"
#include "smem_ralloc_master.h"
#include "smem_ralloc_rpc.h"

namespace ock {
namespace smem {
namespace {
constexpr int64_t SMEMRA_MASTER_DISCOVER_TIMEOUT_MS = 10000; /* 10s */
constexpr uint32_t SMEMRA_CANDIDATE_REGISTER_RETRY = 3U;
}

SmemRallocEntryManager &SmemRallocEntryManager::Instance()
{
    static SmemRallocEntryManager instance;
    return instance;
}

SmemRallocEntryManager::~SmemRallocEntryManager()
{
    // 防止析构函数里面调用UnInitialize
    for (auto &pair : ptr2EntryMap_) {
        pair.second->UnInitialize();
    }
    ptr2EntryMap_.clear();
    for (auto &map : entryIdMap_) {
        map.second->UnInitialize();
    }
    entryIdMap_.clear();
}

Result SmemRallocEntryManager::Initialize(const std::string &storeURL, uint32_t worldSize, uint16_t deviceId,
                                          const smem_ralloc_config_t &config)
{
    std::lock_guard<std::mutex> guard(entryMutex_);
    if (inited_) {
        SM_LOG_WARN("smem ralloc manager has already initialized");
        return SM_OK;
    }

    SM_VALIDATE_RETURN(worldSize != 0, "invalid param, worldSize is 0", SM_INVALID_PARAM);

    storeURL_ = storeURL;
    worldSize_ = worldSize;
    deviceId_ = deviceId;
    config_ = config;

    auto ret = PrepareStore();
    SM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "prepare store failed: " << ret);

    if (config_.autoRanking) {
        ret = AutoRanking();
        SM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "auto ranking failed: " << ret);
    }

    ret = StartControlPlane();
    SM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "start control plane failed: " << ret);

    inited_ = true;
    SM_LOG_INFO("initialize store(" << storeURL << ") world size(" << worldSize << ") device(" << deviceId
                                    << ") rank(" << config_.rankId << ") OK.");
    return SM_OK;
}

int32_t SmemRallocEntryManager::PrepareStore()
{
    SM_ASSERT_RETURN(storeUrlExtraction_.ExtractIpPortFromUrl(storeURL_) == SM_OK, SM_INVALID_PARAM);
    smem_tls_config storeTls{};
    storeTls.tlsEnable = config_.storeTlsConfig.tlsEnable;
    std::copy_n(config_.storeTlsConfig.caPath, SMEM_RALLOC_TLS_PATH_SIZE, storeTls.caPath);
    std::copy_n(config_.storeTlsConfig.crlPath, SMEM_RALLOC_TLS_PATH_SIZE, storeTls.crlPath);
    std::copy_n(config_.storeTlsConfig.certPath, SMEM_RALLOC_TLS_PATH_SIZE, storeTls.certPath);
    std::copy_n(config_.storeTlsConfig.keyPath, SMEM_RALLOC_TLS_PATH_SIZE, storeTls.keyPath);
    std::copy_n(config_.storeTlsConfig.keyPassPath, SMEM_RALLOC_TLS_PATH_SIZE, storeTls.keyPassPath);
    std::copy_n(config_.storeTlsConfig.packagePath, SMEM_RALLOC_TLS_PATH_SIZE, storeTls.packagePath);
    std::copy_n(config_.storeTlsConfig.decrypterLibPath, SMEM_RALLOC_TLS_PATH_SIZE, storeTls.decrypterLibPath);
    StoreFactory::SetTlsInfo(storeTls);
    if (!config_.autoRanking) {
        SM_ASSERT_RETURN(config_.rankId < worldSize_, SM_INVALID_PARAM);
        isStoreServer_ = (config_.rankId == 0 && config_.startConfigStoreServer);
        confStore_ = StoreFactory::CreateStoreByUrl(storeURL_, isStoreServer_, worldSize_, static_cast<int>(config_.rankId));
        SM_ASSERT_RETURN(confStore_ != nullptr, StoreFactory::GetFailedReason());
    } else {
        if (config_.startConfigStoreServer) {
            auto ret = RacingForStoreServer();
            SM_ASSERT_RETURN(ret == SM_OK, ret);
        }

        if (confStore_ == nullptr) {
            confStore_ = StoreFactory::CreateStoreByUrl(storeURL_, false, worldSize_);
            SM_ASSERT_RETURN(confStore_ != nullptr, StoreFactory::GetFailedReason());
        }
    }
    confStore_ = StoreFactory::PrefixStore(confStore_, "RA_");
    return SM_OK;
}

int32_t SmemRallocEntryManager::RacingForStoreServer()
{
    std::string localIp;
    auto success = NetworkEndpointUtil::GetLocalIpWithTarget(storeUrlExtraction_.ip, localIp);
    SM_ASSERT_RETURN(success, SM_ERROR);
    if (localIp != storeUrlExtraction_.ip) {
        return SM_OK;
    }

    confStore_ = StoreFactory::CreateStoreByUrl(storeURL_, true, worldSize_);
    if (confStore_ != nullptr || StoreFactory::GetFailedReason() == SM_RESOURCE_IN_USE) {
        /* confStore_ non-null means local process won the racing and hosts the store server */
        isStoreServer_ = (confStore_ != nullptr);
        return SM_OK;
    }

    return StoreFactory::GetFailedReason();
}

int32_t SmemRallocEntryManager::AutoRanking()
{
    std::vector<uint8_t> rankIdData;
    auto ret = confStore_->GetCoreStore()->Get(AutoRankingStr, rankIdData, SMEM_DEFAUT_WAIT_TIME * SECOND_TO_MILLSEC);
    if (ret == SM_OK && rankIdData.size() == sizeof(uint32_t)) {
        union Transfer {
            uint32_t rankId;
            uint8_t data[4];
        } trans{};
        std::copy_n(rankIdData.begin(), sizeof(trans.data), trans.data);
        config_.rankId = trans.rankId;
        auto tcpConfigStore = Convert<ConfigStore, ConfigStoreManager>(confStore_);
        tcpConfigStore->SetRankId(config_.rankId);
        SM_LOG_INFO("Success to auto ranking rankId: " << trans.rankId << " deviceId: " << deviceId_);
        return SM_OK;
    }
    SM_LOG_ERROR("Failed to auto ranking deviceId: " << deviceId_ << ", ret: " << ret
                                                      << ", dataSize: " << rankIdData.size());
    return SM_ERROR;
}

Result SmemRallocEntryManager::CreateEntryById(uint32_t id, SmemRallocEntryPtr &entry /* out */)
{
    std::lock_guard<std::mutex> guard(entryMutex_);
    /* look up the ralloc entry exists or not with lock */
    SM_ASSERT_RETURN(inited_, SM_NOT_STARTED);
    auto iter = entryIdMap_.find(id);
    if (iter != entryIdMap_.end()) {
        SM_LOG_WARN("create ralloc entry failed as already exists, id: " << id);
        return SM_DUPLICATED_OBJECT;
    }

    /* create new ralloc entry */
    SmemRallocEntryOptions opt{id, config_.rankId, config_.dynamicWorldSize, config_.controlOperationTimeout,
                               config_.role};
    auto store = StoreFactory::PrefixStore(confStore_, std::string("(").append(std::to_string(id)).append(")_"));
    if (store == nullptr) {
        SM_LOG_ERROR("create new prefix store for entity: " << id << " failed");
        return SM_ERROR;
    }

    auto tmpEntry = SmMakeRef<SmemRallocEntry>(opt, store);
    SM_ASSERT_RETURN(tmpEntry != nullptr, SM_NEW_OBJECT_FAILED);

    /* add into set and map */
    entryIdMap_.emplace(id, tmpEntry);
    ptr2EntryMap_.emplace(reinterpret_cast<uintptr_t>(tmpEntry.Get()), tmpEntry);

    /* assign out object ptr */
    entry = tmpEntry;
    SM_LOG_DEBUG("create new ralloc entry success, id: " << id);
    return SM_OK;
}

Result SmemRallocEntryManager::GetEntryByPtr(uintptr_t ptr, SmemRallocEntryPtr &entry)
{
    std::lock_guard<std::mutex> guard(entryMutex_);
    /* look up the ralloc entry exists or not with lock */
    SM_ASSERT_RETURN(inited_, SM_NOT_STARTED);
    auto iter = ptr2EntryMap_.find(ptr);
    if (iter != ptr2EntryMap_.end()) {
        entry = iter->second;
        return SM_OK;
    }

    SM_LOG_DEBUG("not found ralloc entry");
    return SM_OBJECT_NOT_EXISTS;
}

Result SmemRallocEntryManager::GetEntryById(uint32_t id, SmemRallocEntryPtr &entry)
{
    std::lock_guard<std::mutex> guard(entryMutex_);
    /* look up the ralloc entry exists or not with lock */
    SM_ASSERT_RETURN(inited_, SM_NOT_STARTED);
    auto iter = entryIdMap_.find(id);
    if (iter != entryIdMap_.end()) {
        entry = iter->second;
        return SM_OK;
    }

    SM_LOG_DEBUG("not found ralloc entry with id " << id);
    return SM_OBJECT_NOT_EXISTS;
}

Result SmemRallocEntryManager::RemoveEntryByPtr(uintptr_t ptr)
{
    std::lock_guard<std::mutex> guard(entryMutex_);
    /* look up the ralloc entry exists or not with lock */
    SM_ASSERT_RETURN(inited_, SM_NOT_STARTED);
    auto iter = ptr2EntryMap_.find(ptr);
    if (iter == ptr2EntryMap_.end()) {
        SM_LOG_DEBUG("not found ralloc entry");
        return SM_OBJECT_NOT_EXISTS;
    }

    /* assign to a tmp ptr and remove from map */
    auto entry = iter->second;
    ptr2EntryMap_.erase(iter);

    /* remove from id set */
    SM_ASSERT_RETURN(entry != nullptr, SM_ERROR);
    entryIdMap_.erase(entry->Id());

    SM_LOG_DEBUG("remove ralloc entry success, id: " << entry->Id());

    return SM_OK;
}

Result SmemRallocEntryManager::StartControlPlane()
{
    /* resolve local ip towards the store network, used by both rpc listen and candidate register */
    std::string localIp;
    auto success = NetworkEndpointUtil::GetLocalIpWithTarget(storeUrlExtraction_.ip, localIp);
    SM_ASSERT_RETURN(success, SM_ERROR);
    SM_VALIDATE_RETURN(!localIp.empty(), "local ip is empty", SM_ERROR);

    auto port = static_cast<uint32_t>(config_.rpcPortBase) + config_.rankId;
    SM_VALIDATE_RETURN(port <= 0xFFFFU, "rpc port overflow: " << port, SM_INVALID_PARAM);

    SmemRallocRpcEndpoint localEp{};
    localEp.rankId = config_.rankId;
    localEp.port = static_cast<uint16_t>(port);
    (void)strncpy(localEp.ip, localIp.c_str(), sizeof(localEp.ip) - 1);

    {
        std::lock_guard<std::mutex> guard(masterMutex_);
        masterEp_ = localEp;
        masterEp_.rankId = SMEM_RALLOC_INVALID_RANK; /* unknown until discovered */
        (void)memset(masterEp_.ip, 0, sizeof(masterEp_.ip));
    }

    auto ret = SmemRallocRpcService::Instance().Start(localEp, config_.rpcTlsConfig,
                                                      config_.controlOperationTimeout * SECOND_TO_MILLSEC);
    SM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "start rpc service failed: " << ret);

    ret = SmemRallocExecutor::Start();
    SM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "start executor failed: " << ret);

    if (isStoreServer_) {
        /* store server host activates the master service and publishes its rpc endpoint,
         * seeds itself as placement candidate when its role is FAR */
        ret = SmemRallocMasterService::Instance().Start(confStore_, localEp,
                                                        config_.role == SMEM_RALLOC_ROLE_FAR);
        SM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "start master service failed: " << ret);
        std::lock_guard<std::mutex> guard(masterMutex_);
        masterEp_ = localEp;
        if (config_.role == SMEM_RALLOC_ROLE_FAR) {
            /* FAR store host also contributes: its accounting is refreshed via loopback reports */
            StartReporter();
        }
        return SM_OK;
    }

    /* non-master node: discover master endpoint from store, needed by both roles
     * (NEAR sends PLACEMENT to it, FAR registers with it) */
    std::vector<uint8_t> epData;
    ret = confStore_->Get(SMEMRA_RPC_MASTER_STORE_KEY, epData, SMEMRA_MASTER_DISCOVER_TIMEOUT_MS);
    if (ret != SM_OK || epData.size() != sizeof(SmemRallocRpcEndpoint)) {
        /* no master in this deployment, remote policies will be rejected at create time */
        SM_LOG_WARN("master endpoint not found, ret: " << ret << " remote placement disabled");
        return SM_OK;
    }

    SmemRallocRpcEndpoint masterEp{};
    (void)memcpy(&masterEp, epData.data(), sizeof(SmemRallocRpcEndpoint));
    {
        std::lock_guard<std::mutex> guard(masterMutex_);
        masterEp_ = masterEp;
    }

    if (config_.role != SMEM_RALLOC_ROLE_FAR) {
        /* only FAR nodes are placement candidates, NEAR nodes just keep the master endpoint */
        SM_LOG_INFO("node role is NEAR, skip candidate register");
        return SM_OK;
    }

    StartReporter();
    ReportCommittedBytes(SMEMRA_CANDIDATE_REGISTER_RETRY); /* initial register with retries */
    return SM_OK;
}

void SmemRallocEntryManager::StartReporter()
{
    reportIntervalSec_ = mf::MfEnvUtil::GetOptionalUintOrDefault("MF_RALLOC_REPORT_INTERVAL_SEC", 30U);
    poolGraceSec_ = mf::MfEnvUtil::GetOptionalUintOrDefault("MF_RALLOC_POOL_GRACE_SEC", 5U);
    reporterThread_ = std::thread(&SmemRallocEntryManager::ReporterLoop, this);
}

void SmemRallocEntryManager::StopControlPlane()
{
    if (reporterThread_.joinable()) {
        reporterStop_.store(true);
        reporterThread_.join();
    }
    SmemRallocMasterService::Instance().Stop();
    SmemRallocExecutor::Stop();
    SmemRallocRpcService::Instance().Stop();
    std::lock_guard<std::mutex> guard(masterMutex_);
    masterEp_ = {};
    masterEp_.rankId = SMEM_RALLOC_INVALID_RANK;
}

void SmemRallocEntryManager::ReporterLoop()
{
    SM_LOG_INFO("ralloc reporter started, interval: " << reportIntervalSec_ << "s grace: " << poolGraceSec_ << "s");
    while (!reporterStop_.load()) {
        for (uint32_t slept = 0; slept < reportIntervalSec_ && !reporterStop_.load(); slept++) {
            sleep(1U);
        }
        if (reporterStop_.load()) {
            break;
        }
        ReportCommittedBytes(0U);
        ReapEmptyPools();
    }
    SM_LOG_INFO("ralloc reporter stopped");
}

void SmemRallocEntryManager::ReportCommittedBytes(uint32_t retry)
{
    if (!HasMaster()) {
        return;
    }
    uint64_t total = 0;
    {
        std::lock_guard<std::mutex> guard(entryMutex_);
        for (auto &it : entryIdMap_) {
            if (it.second != nullptr) {
                total += it.second->GetCommittedBytes();
            }
        }
    }
    auto &rpc = SmemRallocRpcService::Instance();
    const auto &localEp = rpc.GetLocalEndpoint();
    SmemRallocRpcMsg msg{};
    msg.op = SMEMRA_RPC_OP_REGISTER;
    msg.nodeRank = config_.rankId;
    msg.nodePort = localEp.port;
    (void)strncpy(msg.nodeIp, localEp.ip, sizeof(msg.nodeIp) - 1);
    msg.size = total; /* REGISTER reuses the size field as the committed bytes report */
    for (uint32_t i = 0; i <= retry; i++) {
        auto ret = rpc.SyncCall(GetMasterEndpoint(), msg);
        if (ret == SM_OK && msg.result == SM_OK) {
            SM_LOG_DEBUG("report committed bytes ok, total: " << total);
            return;
        }
        SM_LOG_WARN("report committed bytes failed, ret: " << ret << " result: " << msg.result << " retry: " << i);
        if (i < retry) {
            sleep(1U);
        }
    }
}

void SmemRallocEntryManager::ReapEmptyPools()
{
    std::vector<SmemRallocEntryPtr> victims;
    {
        std::lock_guard<std::mutex> guard(entryMutex_);
        for (auto &it : entryIdMap_) {
            if (it.second != nullptr && it.second->GetRole() == SMEM_RALLOC_ROLE_FAR &&
                it.second->IsPoolEmptyExpired(poolGraceSec_)) {
                victims.push_back(it.second);
            }
        }
    }
    /* teardown outside the entry lock: UnInitialize does a group leave which may take long */
    for (auto &entry : victims) {
        SM_LOG_INFO("reap pool-empty entry, id: " << entry->Id());
        entry->UnInitialize();
        (void)RemoveEntryByPtr(reinterpret_cast<uintptr_t>(entry.Get()));
    }
    if (!victims.empty()) {
        ReportCommittedBytes(0U); /* refresh master accounting right after the teardown */
    }
}

void SmemRallocEntryManager::Destroy()
{
    {
        std::lock_guard<std::mutex> guard(entryMutex_);
        // Uninitialize any entries that were not explicitly destroyed by the caller.
        // This ensures graceful group leave and resource release even if smem_ralloc_destroy()
        // was not called for every handle before smem_ralloc_uninit().
        for (auto &pair : ptr2EntryMap_) {
            if (pair.second != nullptr) {
                pair.second->UnInitialize();
            }
        }
        ptr2EntryMap_.clear();
        entryIdMap_.clear();
    }
    /* stop the control plane (and the reporter thread) without holding the entry lock,
     * the reporter also takes the entry lock while reporting */
    StopControlPlane();
    {
        std::lock_guard<std::mutex> guard(entryMutex_);
        inited_ = false;
        isStoreServer_ = false;
    }
    confStore_ = nullptr;
    StoreFactory::DestroyStore(storeURL_);
}

} // namespace smem
} // namespace ock
