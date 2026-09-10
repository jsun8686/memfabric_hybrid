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
constexpr uint32_t SMEMRA_MASTER_CHANGE_THROTTLE_SEC = 2U; /* min gap between poke-triggered reports */
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
    }

    auto ret = PrepareStore();
    SM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "prepare store failed: " << ret);

    if (config_.autoRanking) {
        ret = AutoRanking();
        SM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "auto ranking failed: " << ret);
    }

    ret = StartControlPlane();
    SM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "start control plane failed: " << ret);

    if (config_.role == SMEM_RALLOC_ROLE_FAR && !masterActivated_.load()) {
        (void)ReportCommittedBytes(SMEMRA_CANDIDATE_REGISTER_RETRY);
    }

    {
        std::lock_guard<std::mutex> guard(entryMutex_);
        inited_ = true;
    }
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
        /* intent shaping stays engine-side: explicit rank-0 host for fixed-rank deployments,
         * a misconfigured host (rank 0 not on the store url host) fails fast inside store
         * creation; whether the intent is honored at all is the store layer's decision */
        const bool isServer = (config_.rankId == 0 && config_.startConfigStoreServer);
        confStore_ = StoreFactory::CreateStoreByUrl(storeURL_, isServer, worldSize_, static_cast<int>(config_.rankId));
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
        /* confStore_ non-null means this process won the racing and hosts the store server
         * (the outcome is queried later via IsLeaderStore, never cached here); a lost racing
         * (RESOURCE_IN_USE, another local process won) falls through to plain client
         * creation in PrepareStore */
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
    localEp_ = localEp;

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

    {
        auto mgr = Convert<ConfigStore, ConfigStoreManager>(confStore_);
        if (mgr != nullptr) {
            /* HA failover: re-election promotes a new store leader, this node then activates
             * its ralloc master; the callback runs on the election thread, work is offloaded */
            mgr->RegisterLeaderPromotionHandler([this]() { OnLeaderPromoted(); });
        }
    }

    {
        auto mgr = Convert<ConfigStore, ConfigStoreManager>(confStore_);
        if (mgr != nullptr && mgr->IsLeaderStore()) {
            /* single activation source: this node currently hosts the store server. tcp
             * stores answer from their fixed creation role (explicit rank-0 host / racing
             * winner), HA stores from the election -- whose initial run happened synchronously
             * inside store creation, before the handler above was registered, hence this
             * one-shot query also catches up the missed initial promotion */
            ret = ActivateMasterOnPromotion();
            SM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "activate master failed: " << ret);
            return SM_OK;
        }
    }

    /* non-master node: discover master endpoint from store, needed by both roles
     * (NEAR sends PLACEMENT to it, FAR registers with it). A miss is not fatal on HA:
     * the master may activate later, the reporter refreshes the endpoint every cycle */
    std::vector<uint8_t> epData;
    ret = confStore_->Get(SMEMRA_RPC_MASTER_STORE_KEY, epData, SMEMRA_MASTER_DISCOVER_TIMEOUT_MS);
    if (ret == SM_OK && epData.size() == sizeof(SmemRallocRpcEndpoint)) {
        SmemRallocRpcEndpoint masterEp{};
        (void)memcpy(&masterEp, epData.data(), sizeof(SmemRallocRpcEndpoint));
        {
            std::lock_guard<std::mutex> guard(masterMutex_);
            masterEp_ = masterEp;
        }
    } else {
        SM_LOG_WARN("master endpoint not found, ret: " << ret << ", remote placement deferred");
    }

    if (config_.role != SMEM_RALLOC_ROLE_FAR) {
        /* only FAR nodes are placement candidates, NEAR nodes just keep the master endpoint */
        SM_LOG_INFO("node role is NEAR, skip candidate register");
        return SM_OK;
    }

    StartReporter();

    /* watch the master key: a master change (restart/failover re-publishes the key) wakes the
     * reporter for an immediate re-register, failover converges in seconds instead of a period */
    ret = confStore_->Watch(
        SMEMRA_RPC_MASTER_STORE_KEY,
        [this](int result, const std::string &key, const std::vector<uint8_t> &value) {
            (void)key;
            OnMasterKeyChanged(result, value);
        },
        masterWatchId_);
    if (ret != SM_OK || masterWatchId_ == UINT32_MAX) {
        /* non-fatal: periodic reporting still refreshes the master view every interval */
        SM_LOG_WARN("watch master key failed, failover re-register falls back to periodic, ret: " << ret);
        masterWatchId_ = UINT32_MAX;
    }


    return SM_OK;
}

Result SmemRallocEntryManager::ActivateMasterOnPromotion()
{
    /* a promotion thread may still be in flight when the control plane is being torn down */
    SM_ASSERT_RETURN(confStore_ != nullptr, SM_NOT_INITIALIZED);
    SubscribeRankDownWatch();
    bool expected = false;
    if (!masterActivated_.compare_exchange_strong(expected, true)) {
        /* already activated: re-publish the endpoint key only, so a failover recovery of this
         * node overwrites the stale master entry possibly left by a dead previous leader */
        std::vector<uint8_t> epData(reinterpret_cast<uint8_t *>(&localEp_),
                                    reinterpret_cast<uint8_t *>(&localEp_) + sizeof(localEp_));
        auto pubRet = confStore_->Set(SMEMRA_RPC_MASTER_STORE_KEY, epData);
        SM_LOG_WARN("master already activated, re-publish master key ret: " << pubRet);
        return pubRet;
    }

    /* store leader activates the master service and publishes its rpc endpoint,
     * seeds itself as placement candidate when its role is FAR */
    auto ret = SmemRallocMasterService::Instance().Start(confStore_, localEp_,
                                                         config_.role == SMEM_RALLOC_ROLE_FAR);
    SM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "start master service failed: " << ret);
    {
        std::lock_guard<std::mutex> guard(masterMutex_);
        masterEp_ = localEp_;
    }
    if (config_.role == SMEM_RALLOC_ROLE_FAR) {
        /* FAR store host also contributes: its accounting is refreshed via loopback reports */
        StartReporter();
    }
    SM_LOG_INFO("master activated, rank: " << localEp_.rankId << " endpoint: " << localEp_.ip << ":"
                                           << localEp_.port);
    return SM_OK;
}

void SmemRallocEntryManager::OnLeaderPromoted()
{
    /* election thread contract: activation does synchronous store writes, keep them off
     * the election loop; serialize promotions by chaining onto the previous thread */
    std::lock_guard<std::mutex> guard(promotionThreadMutex_);
    std::thread prev = std::move(promotionThread_);
    promotionThread_ = std::thread([this, prev = std::move(prev)]() mutable {
        if (prev.joinable()) {
            prev.join();
        }
        (void)ActivateMasterOnPromotion();
    });
}

void SmemRallocEntryManager::SubscribeRankDownWatch()
{
    /* the store pushes rank-down (link broken / heartbeat timeout) to every subscribed
     * link and keeps the waiter alive across events; watches are NOT replayed after a
     * store reconnect, hence this is re-armed on every promotion. The server rejects a
     * duplicate rank watch on the same link, so a failed re-subscribe keeps the previous
     * one (it is still delivering). Callback contract: erase-only, no rpc on this thread. */
    uint32_t wid = UINT32_MAX;
    auto ret = confStore_->Watch(
        WATCH_RANK_LINK_DOWN,
        [](WatchRankType type, uint32_t rank) {
            if (type != WATCH_RANK_LINK_DOWN) {
                return;
            }
            SmemRallocMasterService::Instance().OnRankDown(rank);
        },
        wid);
    if (ret == SM_OK && wid != UINT32_MAX) {
        rankWatchId_ = wid; /* any older subscription dies with its link, no explicit unwatch */
        SM_LOG_INFO("rank-down watch subscribed, wid: " << wid);
    } else {
        SM_LOG_WARN("rank-down watch subscribe failed, ret: " << ret
                      << ", stale-candidate eviction falls back to placement-time prune");
    }
}

void SmemRallocEntryManager::OnMasterKeyChanged(int result, const std::vector<uint8_t> &value)
{
    if (result != SM_OK || value.size() != sizeof(SmemRallocRpcEndpoint)) {
        SM_LOG_WARN("master key watch fired with invalid payload, ret: " << result << " size: " << value.size());
        return;
    }
    SmemRallocRpcEndpoint ep{};
    (void)memcpy(&ep, value.data(), sizeof(SmemRallocRpcEndpoint));
    {
        std::lock_guard<std::mutex> guard(masterMutex_);
        if (ep.rankId == masterEp_.rankId && ep.port == masterEp_.port &&
            strncmp(ep.ip, masterEp_.ip, sizeof(ep.ip)) == 0) {
            return; /* same master, nothing to do */
        }
        masterEp_ = ep;
    }
    SM_LOG_INFO("master endpoint changed, rank: " << ep.rankId << " endpoint: " << ep.ip << ":" << ep.port);
    /* only poke the reporter here, never do rpc on the store watch thread */
    PokeReporter();
}

void SmemRallocEntryManager::PokeReporter()
{
    {
        std::lock_guard<std::mutex> guard(reporterMutex_);
        reporterPoke_ = true;
    }
    reporterCv_.notify_one();
}

void SmemRallocEntryManager::RefreshMasterEndpoint()
{
    if (masterActivated_.load()) {
        return; /* self is the master (hosts the store server), nothing to refresh */
    }
    std::vector<uint8_t> epData;
    auto ret = confStore_->Get(SMEMRA_RPC_MASTER_STORE_KEY, epData, SMEMRA_MASTER_DISCOVER_TIMEOUT_MS);
    if (ret != SM_OK || epData.size() != sizeof(SmemRallocRpcEndpoint)) {
        SM_LOG_WARN("refresh master endpoint failed, ret: " << ret);
        return;
    }
    SmemRallocRpcEndpoint ep{};
    (void)memcpy(&ep, epData.data(), sizeof(SmemRallocRpcEndpoint));
    {
        std::lock_guard<std::mutex> guard(masterMutex_);
        if (ep.rankId == masterEp_.rankId && ep.port == masterEp_.port &&
            strncmp(ep.ip, masterEp_.ip, sizeof(ep.ip)) == 0) {
            return;
        }
        masterEp_ = ep;
    }
    SM_LOG_INFO("master endpoint refreshed, rank: " << ep.rankId << " endpoint: " << ep.ip << ":" << ep.port);
}

void SmemRallocEntryManager::StartReporter()
{
    reportIntervalSec_ = mf::MfEnvUtil::GetOptionalUintOrDefault("MF_RALLOC_REPORT_INTERVAL_SEC", 30U);
    poolGraceSec_ = mf::MfEnvUtil::GetOptionalUintOrDefault("MF_RALLOC_POOL_GRACE_SEC", 5U);
    reporterThread_ = std::thread(&SmemRallocEntryManager::ReporterLoop, this);
}

void SmemRallocEntryManager::StopControlPlane()
{
    if (masterWatchId_ != UINT32_MAX && confStore_ != nullptr) {
        (void)confStore_->Unwatch(masterWatchId_);
        masterWatchId_ = UINT32_MAX;
    }
    if (rankWatchId_ != UINT32_MAX && confStore_ != nullptr) {
        (void)confStore_->Unwatch(rankWatchId_);
        rankWatchId_ = UINT32_MAX;
    }
    if (reporterThread_.joinable()) {
        reporterStop_.store(true);
        {
            std::lock_guard<std::mutex> guard(reporterMutex_);
            reporterPoke_ = true;
        }
        reporterCv_.notify_all();
        reporterThread_.join();
    }
    {
        std::lock_guard<std::mutex> guard(promotionThreadMutex_);
        if (promotionThread_.joinable()) {
            promotionThread_.join();
        }
    }
    masterActivated_.store(false);
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
    auto lastReport = std::chrono::steady_clock::now();
    while (!reporterStop_.load()) {
        bool poked = false;
        {
            std::unique_lock<std::mutex> lock(reporterMutex_);
            reporterCv_.wait_for(lock, std::chrono::seconds(reportIntervalSec_),
                                 [this]() { return reporterPoke_ || reporterStop_.load(); });
            poked = reporterPoke_;
            reporterPoke_ = false;
        }
        if (reporterStop_.load()) {
            break;
        }
        if (poked) {
            /* absorb master flapping: keep a min gap between poke-triggered reports */
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - lastReport).count();
            if (elapsed < static_cast<int64_t>(SMEMRA_MASTER_CHANGE_THROTTLE_SEC)) {
                sleep(SMEMRA_MASTER_CHANGE_THROTTLE_SEC - static_cast<uint32_t>(elapsed));
                if (reporterStop_.load()) {
                    break;
                }
            }
        }
        auto reported = ReportCommittedBytes(0U);
        lastReport = std::chrono::steady_clock::now();
        if (!reported && poked) {
            /* the new master may not be activated yet when the watch fires, one bounded retry */
            sleep(SMEMRA_MASTER_CHANGE_THROTTLE_SEC);
            if (reporterStop_.load()) {
                break;
            }
            (void)ReportCommittedBytes(0U);
            lastReport = std::chrono::steady_clock::now();
        }
        ReapEmptyPools();
    }
    SM_LOG_INFO("ralloc reporter stopped");
}

bool SmemRallocEntryManager::ReportCommittedBytes(uint32_t retry)
{
    if (!HasMaster()) {
        /* master may activate later (HA election / init race): re-read before giving
         * up this cycle so the reporter self-heals instead of dead-ending */
        RefreshMasterEndpoint();
        if (!HasMaster()) {
            return false;
        }
    }
    uint64_t total = 0;
    uint64_t deviceTotal = 0;
    {
        std::lock_guard<std::mutex> guard(entryMutex_);
        for (auto &it : entryIdMap_) {
            if (it.second != nullptr) {
                total += it.second->GetCommittedBytes();
                deviceTotal += it.second->GetDeviceCommittedBytes();
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
    msg.size = total; /* REGISTER reuses the size field as the HOST committed bytes report */
    msg.deviceCommittedBytes = deviceTotal;
    for (uint32_t i = 0; i <= retry; i++) {
        auto ret = rpc.SyncCall(GetMasterEndpoint(), msg);
        if (ret == SM_OK && msg.result == SM_OK) {
            SM_LOG_DEBUG("report committed bytes ok, total: " << total << " deviceTotal: " << deviceTotal);
            return true;
        }
        SM_LOG_WARN("report committed bytes failed, ret: " << ret << " result: " << msg.result << " retry: " << i);
        if (i < retry) {
            sleep(1U);
        }
    }
    /* the cached endpoint may point to a dead master (restart/failover), refresh it so the
     * next cycle or the master-key watch converges to the new master */
    RefreshMasterEndpoint();
    return false;
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
    }
    confStore_ = nullptr;
    StoreFactory::DestroyStore(storeURL_);
}

} // namespace smem
} // namespace ock
