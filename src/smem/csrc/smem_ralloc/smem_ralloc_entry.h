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
#include <condition_variable>
#include <map>
#include <set>
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

    Result DataCopyBatch(smem_ralloc_batch_copy_params *params, uint32_t flags);

    Result Wait();

    Result RegisterMem(uint64_t addr, uint64_t size);

    Result UnRegisterMem(uint64_t addr);

    bool IsUserRegistered(uint64_t addr, uint64_t size);

    Result SetGroupEventHandler(smem_ralloc_group_event_cb cb, void *context);

    uint32_t Id() const;

    uint32_t GetRankId() const;

    uint32_t GetEntityId() const;

    uint32_t GetRankIdByGva(void *gva);

    const hybm_options &GetCoreOptions() const;

    void *GetHostGvaAddress() const;

    /* fill info with the first local block, {SMEM_RALLOC_INVALID_RANK, null} if local slot is empty */
    Result GetLocalMemInfo(smem_ralloc_mem_info_t *info);

    /* current committed size of one rank's slot, snapshot of local imported state, 0 if empty */
    uint64_t GetMemSizeByRank(uint32_t rank, smem_ralloc_mem_type_t memType = SMEM_RALLOC_MEM_TYPE_HOST);

    /* slot base address of one rank on the requested media window: resolved from the entity's
     * committed ranges (truth), NOT from window arithmetic -- a peer's slot may sit off its
     * arithmetic position when the peer process' LVA layout drifted inside the unified window;
     * null when the slot has nothing committed */
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

    /* join-vs-reap arbitration: an active join cancels the pending pool-empty reap for
     * this cycle; an entry taken over by the reaper (or UnInitialize) rejects new joins
     * fast instead of hanging on a torn-down group. BeginJoinActive also clears the
     * pool-empty mark at join START (not after), closing the reap-vs-join window */
    bool BeginJoinActive();
    void EndJoinActive();
    /* atomically (with joins) claim the entry for teardown: false when a join is active */
    bool MarkTearingDownIfIdle();
    /* true when no live NEAR member is left in the pool (same rule as EvaluatePoolEmpty) */
    bool PoolEmpty() const;

    /* record a member rank as departed (its store link broke): the rank no longer blocks
     * pool-empty evaluation -- this is what un-blocks the reap after a failed join left
     * a phantom NEAR member in the group view; a later re-join of the same rank cancels
     * the departure (UpdateMemberRole). No rpc on this path (store watch thread). */
    void MarkMemberDeparted(uint32_t rk);

    /* re-run the pool-empty evaluation (idempotent, skips while a join is in flight):
     * the reaper calls it every cycle so a silently cleared mark or a missed leave
     * event still converges instead of dead-ending the reap */
    void RefreshPoolEmpty();

    /* pool-empty countdown state for the reporter fast path: marked + seconds until
     * the grace expires (0 when already expired or not marked) */
    bool PoolEmptyMarked() const;
    uint32_t PoolEmptyRemainingSec(uint64_t graceSec) const;

    /* lifecycle of the executor-driven first JOIN_ALLOC (create branch): mark RUNNING
     * before Initialize, mark done after Join succeeded / on every failure path; extend
     * calls park on the cv instead of failing fast while the first slice builds */
    void MarkBootstrapRunning();

    void MarkBootstrapDone(bool ok);

    /* park until a concurrent bootstrap finishes; SM_OK when no bootstrap is running
     * (never started or READY), SM_NOT_INITIALIZED when it failed or timed out */
    Result WaitForBootstrap();

private:
    bool AddrInHostGva(const void *address, uint64_t size);

    bool AddrInDeviceGva(const void *address, uint64_t size);

    Result PublishUserMrTable();

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

    /* committed ranges of the whole unified window [base, base + slotSize * rankCount):
     * the envelope peers may actually sit anywhere inside once their own LVA layout
     * drifted, so the per-rank arithmetic slice is not a valid query bound */
    bool QueryWindowRanges(smem_ralloc_mem_type_t memType, std::vector<hybm_va_range> &ranges);
    /* aggregate the committed extent of one rank inside the window: baseOut = lowest
     * range gva (the slot base), extentOut = max end - baseOut; false when uncommitted */
    bool QueryRankSlot(uint32_t rank, smem_ralloc_mem_type_t memType, uint64_t &baseOut, uint64_t &extentOut);

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
    std::map<uint64_t, std::pair<uint64_t, hybm_mem_slice_t>> registedSlice_;

    struct UserMrInfo {
        uint64_t devAddr; /* GVA base of the region, == the registration key and the kernel
                           * match key; equals the device-dma base for HBM regions (P1: the only
                           * supported user-memory class, regAddress == addr) */
        uint64_t regAddress; /* device-dma base the MR was registered under: == devAddr for HBM,
                              * the HalHostRegister iova for 4K-aligned host-DRAM regions; the
                              * kernel derives the SGE as regAddress + (localAddr - devAddr) */
        uint64_t size;
        uint32_t lkey;
        uint32_t rkey;
    };
    std::map<uint64_t, UserMrInfo> userMrs_;

    std::mutex eventCbMutex_;
    smem_ralloc_group_event_cb eventCb_ = nullptr;
    void *eventCbCtx_ = nullptr;

    /* pool lifecycle bookkeeping: role of every member rank, maintained from group events */
    mutable std::mutex roleMutex_;
    std::map<uint32_t, smem_ralloc_role_t> memberRoles_;
    std::set<uint32_t> departedRanks_; /* store-link-confirmed gone, never blocks reap */
    bool joinActive_ = false;   /* guarded by roleMutex_: a JoinHandle is in flight */
    bool tearingDown_ = false;  /* guarded by roleMutex_: reaper/UnInitialize owns the entry */
    std::atomic<uint64_t> poolEmptySinceUs_{0};
    std::atomic<uint64_t> committedBytes_{0};
    std::atomic<uint64_t> deviceCommittedBytes_{0};

    /* first-JOIN_ALLOC bootstrap state, see MarkBootstrapRunning; guarded by its own mutex
     * because the bootstrap thread never takes mutex_ and extend waiters must not queue
     * behind a mutex_ held for a whole slice materialization */
    enum class BootstrapState { NONE, RUNNING, READY, FAILED };
    std::mutex bsMutex_;
    std::condition_variable bsCv_;
    BootstrapState bsState_ = BootstrapState::NONE;
};
using SmemRallocEntryPtr = SmRef<SmemRallocEntry>;

inline uint32_t SmemRallocEntry::Id() const
{
    return options_.id;
}

inline uint32_t SmemRallocEntry::GetRankId() const
{
    return options_.rank;
}

inline uint32_t SmemRallocEntry::GetEntityId() const
{
    return (Id() << 1U) + 1U;
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
