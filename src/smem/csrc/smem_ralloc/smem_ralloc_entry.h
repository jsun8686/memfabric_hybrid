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
#ifndef MEMFABRIC_HYBRID_SMEM_RALLOC_ENTRY_H
#define MEMFABRIC_HYBRID_SMEM_RALLOC_ENTRY_H

#include "hybm_def.h"
#include "smem_common_includes.h"
#include "smem_config_store.h"
#include "smem_net_group_engine.h"
#include "smem_ralloc.h"

#include <atomic>
#include <map>
#include <vector>

namespace ock {
namespace smem {
struct SmemRallocEntryOptions {
    uint32_t id;
    uint32_t rank;
    uint32_t rankSize;
    uint64_t controlOperationTimeout;
    smem_ralloc_role_t role;
};

class SmemRallocEntry : public SmReferable {
public:
    explicit SmemRallocEntry(const SmemRallocEntryOptions &options, const StorePtr &store)
        : options_(options), configStore_(store), coreOptions_{}, entityInfo_{}
    {}

    ~SmemRallocEntry() override
    {
        UnInitialize();
    };

    int32_t Initialize(const hybm_options &options);

    void UnInitialize();

    Result Join(uint32_t flags);

    Result ExtendLocalMem(smem_ralloc_mem_type_t memType, uint64_t size, smem_ralloc_mem_info_t *info);

    Result DataCopy(const void *src, void *dest, uint64_t size, uint32_t flags);

    Result Wait();

    Result SetGroupEventHandler(smem_ralloc_group_event_cb cb, void *context);

    uint32_t Id() const;

    uint32_t GetRankIdByGva(void *gva);

    const hybm_options &GetCoreOptions() const;

    void *GetHostGvaAddress() const;

    /* fill info with the first local block, {SMEM_RALLOC_INVALID_RANK, null} if local slot is empty */
    Result GetLocalMemInfo(smem_ralloc_mem_info_t *info);

    /* current committed size of one rank's slot, snapshot of local imported state, 0 if empty */
    uint64_t GetMemSizeByRank(uint32_t rank, smem_ralloc_mem_type_t memType = SMEM_RALLOC_MEM_TYPE_HOST);

    /* slot base address of one rank on the requested media window, null if invalid */
    void *GetMemPtrByRank(uint32_t rank, smem_ralloc_mem_type_t memType = SMEM_RALLOC_MEM_TYPE_HOST);

    /* snapshot of the ranks currently in the dynamic group, includes self, empty if not joined */
    std::vector<uint32_t> GetGroupRanks();

    smem_ralloc_role_t GetRole() const;

    /* total committed bytes of the local HOST slot, authoritative value for master accounting */
    uint64_t GetCommittedBytes() const;

    /* total committed bytes of the local DEVICE slot, authoritative value for master accounting */
    uint64_t GetDeviceCommittedBytes() const;

    /* true when a FAR entry has been marked pool-empty (no NEAR member left) for longer
     * than graceSec, ready for self teardown by the manager reaper */
    bool IsPoolEmptyExpired(uint64_t graceSec) const;

private:
    bool AddrInHostGva(const void *address, uint64_t size);

    bool AddrInDeviceGva(const void *address, uint64_t size);

    Result CheckJoined() const;

    Result CreateGlobalTeam(uint32_t rankSize, uint32_t rankId);

    Result JoinHandle(uint32_t rk);
    Result UpdateHandle(uint32_t rk);
    Result GroupOpBarrier(int32_t input);
    Result LeaveHandle(uint32_t rk);
    void InvokeEventCb(uint32_t rankId, smem_ralloc_group_event_t event);
    Result WriteSelfRoleKey();
    smem_ralloc_role_t ReadRoleKey(uint32_t rank);
    void UpdateMemberRole(uint32_t rk);
    void EvaluatePoolEmpty();

private:
    /* hot used variables */
    bool inited_ = false;
    std::mutex mutex_;
    SmemGroupEnginePtr globalGroup_ = nullptr;
    hybm_entity_t entity_ = nullptr;
    void *hostGva_ = nullptr;
    void *deviceGva_ = nullptr; /* device/HBM window base, null when the pool has no HBM window */

    /* non-hot used variables */
    SmemRallocEntryOptions options_;
    hybm_options coreOptions_;
    StorePtr configStore_;
    hybm_exchange_info entityInfo_;
    std::vector<hybm_mem_slice_t> slices_;
    std::vector<hybm_exchange_info> sliceInfos_;

    std::mutex eventCbMutex_;
    smem_ralloc_group_event_cb eventCb_ = nullptr;
    void *eventCbCtx_ = nullptr;

    /* pool lifecycle bookkeeping: role of every member rank, maintained from group events */
    mutable std::mutex roleMutex_;
    std::map<uint32_t, smem_ralloc_role_t> memberRoles_;
    std::atomic<uint64_t> poolEmptySinceUs_{0};
    std::atomic<uint64_t> committedBytes_{0};
    std::atomic<uint64_t> deviceCommittedBytes_{0};
};
using SmemRallocEntryPtr = SmRef<SmemRallocEntry>;

inline uint32_t SmemRallocEntry::Id() const
{
    return options_.id;
}

inline const hybm_options &SmemRallocEntry::GetCoreOptions() const
{
    return coreOptions_;
}

inline void *SmemRallocEntry::GetHostGvaAddress() const
{
    return hostGva_;
}

inline smem_ralloc_role_t SmemRallocEntry::GetRole() const
{
    return options_.role;
}

inline uint64_t SmemRallocEntry::GetCommittedBytes() const
{
    return committedBytes_.load();
}

inline uint64_t SmemRallocEntry::GetDeviceCommittedBytes() const
{
    return deviceCommittedBytes_.load();
}

} // namespace smem
} // namespace ock

#endif // MEMFABRIC_HYBRID_SMEM_RALLOC_ENTRY_H
