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
#ifndef MEMFABRIC_HYBRID_SMEM_RALLOC_ENTRY_MANAGER_H
#define MEMFABRIC_HYBRID_SMEM_RALLOC_ENTRY_MANAGER_H

#include <atomic>
#include <condition_variable>
#include <string>
#include <thread>
#include "smem_net_common.h"
#include "smem_ralloc.h"
#include "smem_ralloc_entry.h"
#include "smem_ralloc_rpc_def.h"
#include "smem_config_store.h"

namespace ock {
namespace smem {

class SmemRallocEntryManager {
public:
    static SmemRallocEntryManager &Instance();

    SmemRallocEntryManager() = default;
    ~SmemRallocEntryManager();

    SmemRallocEntryManager(const SmemRallocEntryManager &) = delete;
    SmemRallocEntryManager(SmemRallocEntryManager &&) = delete;
    SmemRallocEntryManager &operator=(const SmemRallocEntryManager &other) = delete;
    SmemRallocEntryManager &operator=(SmemRallocEntryManager &&other) = delete;

    Result Initialize(const std::string &storeURL, uint32_t worldSize, uint16_t deviceId,
                      const smem_ralloc_config_t &config);

    Result CreateEntryById(uint32_t id, SmemRallocEntryPtr &entry);
    Result GetEntryByPtr(uintptr_t ptr, SmemRallocEntryPtr &entry);
    Result GetEntryById(uint32_t id, SmemRallocEntryPtr &entry);
    Result RemoveEntryByPtr(uintptr_t ptr);

    void Destroy();

    inline uint32_t GetRankId() const
    {
        return config_.rankId;
    }

    inline uint32_t GetWorldSize() const
    {
        return worldSize_;
    }

    inline uint16_t GetDeviceId() const
    {
        return deviceId_;
    }

    inline std::string GetHcomUrl() const
    {
        return config_.hcomUrl;
    }

    inline smem_ralloc_tls_config GetHcomTlsOption() const
    {
        return config_.hcomTlsConfig;
    }

    inline uint16_t GetRpcPortBase() const
    {
        return config_.rpcPortBase;
    }

    inline const smem_ralloc_config_t &GetConfig() const
    {
        return config_;
    }

    /* master rpc endpoint of the deployment, rankId is SMEM_RALLOC_INVALID_RANK when absent */
    inline SmemRallocRpcEndpoint GetMasterEndpoint() const
    {
        std::lock_guard<std::mutex> guard(masterMutex_);
        return masterEp_;
    }

    inline bool HasMaster() const
    {
        std::lock_guard<std::mutex> guard(masterMutex_);
        return masterEp_.rankId != SMEM_RALLOC_INVALID_RANK && masterEp_.ip[0] != '\0';
    }

    /* re-read the master endpoint from the store, self-heal after master restart/failover */
    void RefreshMasterEndpoint();

    /* this process hosts the config store server (and then the ralloc master service) */
    inline bool IsStoreServer() const
    {
        return isStoreServer_;
    }

private:
    int32_t PrepareStore();
    int32_t RacingForStoreServer();
    int32_t AutoRanking();
    Result StartControlPlane();
    void StopControlPlane();
    /* periodic thread on FAR nodes: reports committed bytes to the master and reaps
     * pool-empty executor entries after the grace period */
    void StartReporter();
    void ReporterLoop();
    void OnMasterKeyChanged(int result, const std::vector<uint8_t> &value);
    bool ReportCommittedBytes(uint32_t retry);
    void ReapEmptyPools();

private:
    std::mutex entryMutex_;
    std::map<uintptr_t, SmemRallocEntryPtr> ptr2EntryMap_; /* lookup entry by ptr */
    std::map<uint32_t, SmemRallocEntryPtr> entryIdMap_;    /* deduplicate entry by id */
    smem_ralloc_config_t config_{};
    std::string storeURL_;
    uint32_t worldSize_{0};
    uint16_t deviceId_{0};
    bool inited_ = false;
    bool isStoreServer_ = false;
    std::thread reporterThread_;
    std::atomic<bool> reporterStop_{false};
    uint32_t reportIntervalSec_ = 30U;
    uint32_t poolGraceSec_ = 5U;
    std::mutex reporterMutex_;
    std::condition_variable reporterCv_;
    bool reporterPoke_ = false;
    uint32_t masterWatchId_ = UINT32_MAX;
    mutable std::mutex masterMutex_;
    SmemRallocRpcEndpoint masterEp_{};
    UrlExtraction storeUrlExtraction_;
    StorePtr confStore_ = nullptr;
};

} // namespace smem
} // namespace ock

#endif // MEMFABRIC_HYBRID_SMEM_RALLOC_ENTRY_MANAGER_H
