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
#include "smem_ralloc_entry.h"

#include <algorithm>
#include <chrono>

#include "hybm_def.h"
#include "hybm_big_mem.h"
#include "hybm_data_op.h"
#include "mf_env_util.h"
#include "mf_monotonic_time.h"
#include "smem_store_factory.h"
#include "mf_fault_injection_point.h"
#include "smem_ralloc_rpc_def.h"

namespace ock {
namespace smem {

int32_t SmemRallocEntry::Initialize(const hybm_options &options)
{
    if (inited_) {
        return SM_OK;
    }
    uint32_t flags = 0;
    hybm_entity_t entity = nullptr;
    hybm_mem_slice_t slice = nullptr;
    Result ret = SM_ERROR;

    SM_LOG_ERROR_RETURN_IT_IF_NOT_OK(CreateGlobalTeam(options.rankCount, options.rankId), "create global team failed");

    do {
        entity = hybm_create_entity((Id() << 1) + 1U, &options, flags);
        if (entity == nullptr) {
            SM_LOG_ERROR("create entity failed");
            ret = SM_ERROR;
            break;
        }

        ret = hybm_reserve_mem_space(entity, flags);
        if (ret != 0) {
            SM_LOG_ERROR("reserve mem failed, result: " << ret);
            hybm_destroy_entity(entity, flags);
            ret = SM_ERROR;
            break;
        }
        entity_ = entity;

        hybm_exchange_info dramSliceInfo{};
        if (options.maxDRAMSize > 0 && options.hostVASpace > 0) {
            slice = hybm_alloc_local_memory(entity, HYBM_MEM_TYPE_HOST, options.hostVASpace, flags);
            if (slice == nullptr) {
                SM_LOG_ERROR("alloc local host mem failed, size: " << options.hostVASpace);
                ret = SM_ERROR;
                break;
            }
            slices_.push_back(slice);

            ret = hybm_export(entity, slice, flags, &dramSliceInfo);
            if (ret != 0) {
                SM_LOG_ERROR("hybm export host slice failed, result: " << ret);
                break;
            }
            sliceInfos_.push_back(dramSliceInfo);
        }

        hybm_exchange_info deviceSliceInfo{};
        if (options.maxHBMSize > 0 && options.deviceVASpace > 0) {
            slice = hybm_alloc_local_memory(entity, HYBM_MEM_TYPE_DEVICE, options.deviceVASpace, flags);
            if (slice == nullptr) {
                SM_LOG_ERROR("alloc local device mem failed, size: " << options.deviceVASpace);
                ret = SM_ERROR;
                break;
            }
            slices_.push_back(slice);

            ret = hybm_export(entity, slice, flags, &deviceSliceInfo);
            if (ret != 0) {
                SM_LOG_ERROR("hybm export device slice failed, result: " << ret);
                break;
            }
            sliceInfos_.push_back(deviceSliceInfo);
        }

        bzero(&entityInfo_, sizeof(hybm_exchange_info));
        ret = hybm_export(entity, nullptr, HYBM_FLAG_EXPORT_ENTITY, &entityInfo_);
        if (ret != 0) {
            SM_LOG_ERROR("hybm entity export failed, result: " << ret);
            break;
        }
    } while (0);

    if (ret != 0) {
        UnInitialize();
        globalGroup_ = nullptr;
        return ret;
    }

    coreOptions_ = options;
    hostGva_ = hybm_get_memory_ptr(entity, HYBM_MEM_TYPE_HOST);
    deviceGva_ = hybm_get_memory_ptr(entity, HYBM_MEM_TYPE_DEVICE);
    committedBytes_.store(options.hostVASpace);
    deviceCommittedBytes_.store(options.deviceVASpace);
    inited_ = true;
    return 0;
}

void SmemRallocEntry::UnInitialize()
{
    if (!inited_) {
        return;
    }
    if (entity_ == nullptr) {
        return;
    }
    // Perform a graceful group leave so that peer ranks can synchronously clean up
    // their imported state via LeaveHandle(). This must happen before the local entity
    // is destroyed because the leave callback on the peer side still references entity_.
    if (globalGroup_ != nullptr && globalGroup_->IsJoined()) {
        auto ret = globalGroup_->GroupLeave();
        if (ret != SM_OK) {
            SM_LOG_WARN("group leave failed during uninitialize, ret: " << ret);
        }
    }
    // Stop the group engine listen thread before releasing the entity.
    globalGroup_ = nullptr;

    uint32_t flags = 0;
    for (auto slice : slices_) {
        hybm_free_local_memory(entity_, slice, 1, flags);
    }
    slices_.clear();
    sliceInfos_.clear();
    hybm_unreserve_mem_space(entity_, flags);
    hybm_destroy_entity(entity_, flags);
    entity_ = nullptr;
    hostGva_ = nullptr;
    deviceGva_ = nullptr;
    committedBytes_.store(0);
    deviceCommittedBytes_.store(0);
    poolEmptySinceUs_.store(0);
    inited_ = false;
}

Result SmemRallocEntry::GroupOpBarrier(int32_t input)
{
    int32_t remoteRet = input;
    int32_t ret = globalGroup_->GroupGatherResult(input, remoteRet);
    if (ret != SM_OK) {
        SM_LOG_ERROR("join barrier failed, result: " << ret);
        return ret;
    }
    if (remoteRet != SM_OK) {
        SM_LOG_ERROR("join barrier, get remote result: " << remoteRet);
        return SM_ERROR;
    }
    return SM_OK;
}

Result SmemRallocEntry::JoinHandle(uint32_t rk)
{
    SM_ASSERT_RETURN(inited_, SM_NOT_INITIALIZED);
    SM_LOG_INFO("do join func, local_rk: " << options_.rank << " receive_rk: " << rk
                                           << ", rank size is: " << globalGroup_->GetRankSize());

    uint32_t unitSize = sizeof(hybm_exchange_info);
    std::string localInfo;
    if (rk == options_.rank) {
        localInfo = std::string((char *)&entityInfo_, sizeof(hybm_exchange_info));
        for (auto sliceInfo : sliceInfos_) {
            if (sliceInfo.descLen > 0) {
                localInfo += std::string((char *)&sliceInfo, sizeof(hybm_exchange_info));
            }
        }
    }
    std::unordered_map<uint32_t, std::string> allInfo;
    std::vector<uint32_t> joined;
    int32_t ret = globalGroup_->GroupGatherPrefixKey(rk, localInfo, allInfo);
    SM_VALIDATE_RETURN(ret == SM_OK, "gather prefix info failed, ret:" << ret, ret);
    hybm_exchange_info info;
    std::vector<hybm_exchange_info> entityInfos;
    std::vector<hybm_exchange_info> sliceInfos;

    for (auto &it : allInfo) {
        if (it.first == options_.rank) {
            continue;
        }
        if (it.second.length() % unitSize != 0) {
            SM_LOG_ERROR("receive exchange info size is invalid!, size:" << it.second.length() << " rank:" << it.first);
            ret = SM_INVALID_PARAM;
            goto join_exit;
        }
        uint32_t num = it.second.length() / unitSize;
        joined.push_back(it.first);
        for (uint32_t i = 0; i < num; i++) {
            (void)std::copy_n(it.second.c_str() + i * unitSize, unitSize, (char *)&info);
            if (i == 0) {
                entityInfos.push_back(info);
            } else {
                sliceInfos.push_back(info);
            }
        }
    }

    if (!entityInfos.empty()) {
        ret = hybm_import(entity_, entityInfos.data(), entityInfos.size(), nullptr, HYBM_FLAG_EXPORT_ENTITY);
        if (ret != SM_OK) {
            SM_LOG_ERROR("hybm import entity failed, result: " << ret << " local_rank:" << options_.rank);
            goto join_exit;
        }
    }

    if (!sliceInfos.empty()) {
        ret = hybm_import(entity_, sliceInfos.data(), sliceInfos.size(), nullptr, 0);
        if (ret != SM_OK) {
            SM_LOG_ERROR("hybm import slice failed, result: " << ret << " local_rank:" << options_.rank);
            goto join_exit;
        }
    }

    ret = GroupOpBarrier(ret);
    if (ret != SM_OK) {
        SM_LOG_ERROR("hybm barrier before mmap failed, result: " << ret);
        goto rollback_exit;
    }

    FIP_START(MMAP, &ret)
    ret = hybm_mmap(entity_, 0);
    FIP_END;
    if (ret != SM_OK) {
        SM_LOG_ERROR("hybm mmap failed, result: " << ret);
    }

join_exit:
    ret = GroupOpBarrier(ret);
    if (ret != SM_OK) {
        SM_LOG_ERROR("hybm barrier after mmap failed, result: " << ret);
        goto rollback_exit;
    }

    SM_LOG_INFO("end join func, local_rk: " << options_.rank << " receive_rk: " << rk << " receive_info_num:"
                                            << allInfo.size() << ", rank size is: " << globalGroup_->GetRankSize());
    UpdateMemberRole(rk);
    InvokeEventCb(rk, SMEM_RALLOC_GROUP_EVENT_JOIN);
    return SM_OK;

rollback_exit:
    for (auto &rks : joined) {
        hybm_remove_imported(entity_, rks, 0);
    }
    return ret;
}

Result SmemRallocEntry::UpdateHandle(uint32_t rk)
{
    SM_ASSERT_RETURN(inited_, SM_NOT_INITIALIZED);
    SM_LOG_INFO("do update func, local_rk: " << options_.rank << " receive_rk: " << rk
                                             << ", rank size is: " << globalGroup_->GetRankSize());

    uint32_t unitSize = sizeof(hybm_exchange_info);
    std::string xinfo;
    if (rk == options_.rank) {
        xinfo = std::string((char *)&sliceInfos_.back(), sizeof(hybm_exchange_info));
    }

    int32_t ret = globalGroup_->GroupBarrierPrefixKey(rk, xinfo);
    SM_VALIDATE_RETURN(ret == SM_OK, "barrier prefix info failed, ret:" << ret, ret);
    if (rk != options_.rank) {
        hybm_exchange_info info;
        if (xinfo.length() % unitSize != 0) {
            SM_LOG_ERROR("receive exchange info size is invalid!, size:" << xinfo.length() << " rank:" << rk);
            ret = SM_INVALID_PARAM;
            goto update_exit;
        }
        uint32_t num = xinfo.length() / unitSize;
        for (uint32_t i = 0; i < num; i++) {
            (void)std::copy_n(xinfo.c_str() + i * unitSize, unitSize, (char *)&info);
            ret = hybm_import(entity_, &info, 1U, nullptr, (i == 0 ? HYBM_FLAG_EXPORT_ENTITY : 0));
            if (ret != SM_OK) {
                SM_LOG_ERROR("hybm import failed, result: " << ret << " remote_rank:" << rk
                                                            << " local_rank:" << options_.rank);
                goto update_exit;
            }
        }

        FIP_START(MMAP, &ret)
        ret = hybm_mmap(entity_, 0);
        FIP_END;
        if (ret != SM_OK) {
            SM_LOG_ERROR("hybm mmap failed, result: " << ret);
        }
    }

update_exit:
    ret = GroupOpBarrier(ret);
    if (ret != SM_OK) {
        SM_LOG_ERROR("hybm barrier after mmap failed, result: " << ret);
        // todo: how to rollback mmap
        return ret;
    }

    SM_LOG_INFO("end update func, local_rk: " << options_.rank << " receive_rk: " << rk
                                              << ", rank size is: " << globalGroup_->GetRankSize());
    return SM_OK;
}

Result SmemRallocEntry::LeaveHandle(uint32_t rk)
{
    SM_LOG_INFO("do leave func, receive_rk: " << rk);
    SM_ASSERT_RETURN(inited_, SM_NOT_INITIALIZED);
    auto ret = hybm_remove_imported(entity_, rk, 0);
    if (ret != 0) {
        SM_LOG_ERROR("hybm leave failed, result: " << ret);
        return SM_ERROR;
    }
    {
        std::lock_guard<std::mutex> guard(roleMutex_);
        memberRoles_.erase(rk);
    }
    EvaluatePoolEmpty();
    InvokeEventCb(rk, SMEM_RALLOC_GROUP_EVENT_LEAVE);
    return SM_OK;
}

void SmemRallocEntry::InvokeEventCb(uint32_t rankId, smem_ralloc_group_event_t event)
{
    std::unique_lock<std::mutex> locker{eventCbMutex_};
    auto cb = eventCb_;
    auto ctx = eventCbCtx_;
    locker.unlock();

    if (cb != nullptr) {
        (*cb)(reinterpret_cast<void *>(this), rankId, event, ctx);
    }
}

Result SmemRallocEntry::WriteSelfRoleKey()
{
    auto role = static_cast<uint32_t>(options_.role);
    std::vector<uint8_t> val(sizeof(uint32_t));
    (void)memcpy(val.data(), &role, sizeof(uint32_t));
    std::string key = std::string(SMEMRA_POOL_ROLE_KEY_PREFIX) + std::to_string(options_.rank);
    auto ret = configStore_->Set(key, val);
    SM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "set role key failed, key: " << key);
    return SM_OK;
}

smem_ralloc_role_t SmemRallocEntry::ReadRoleKey(uint32_t rank)
{
    std::string key = std::string(SMEMRA_POOL_ROLE_KEY_PREFIX) + std::to_string(rank);
    for (uint32_t i = 0; i < 3U; i++) {
        std::vector<uint8_t> val;
        if (configStore_->Get(key, val, SMEMRA_ROLE_KEY_READ_TIMEOUT_MS) == SM_OK &&
            val.size() == sizeof(uint32_t)) {
            uint32_t role = 0;
            (void)memcpy(&role, val.data(), sizeof(uint32_t));
            if (role < static_cast<uint32_t>(SMEM_RALLOC_ROLE_BUTT)) {
                return static_cast<smem_ralloc_role_t>(role);
            }
            break;
        }
    }
    /* fail-safe: unknown role counts as NEAR so pool teardown never fires early */
    return SMEM_RALLOC_ROLE_NEAR;
}

void SmemRallocEntry::UpdateMemberRole(uint32_t rk)
{
    if (rk == options_.rank) {
        /* own join completed, seed roles of all existing members */
        std::vector<uint32_t> ranks;
        globalGroup_->GetMemberRanks(ranks);
        std::map<uint32_t, smem_ralloc_role_t> roles;
        for (auto r : ranks) {
            if (r != options_.rank) {
                roles.emplace(r, ReadRoleKey(r));
            }
        }
        std::lock_guard<std::mutex> guard(roleMutex_);
        memberRoles_ = roles;
        poolEmptySinceUs_.store(0);
        return;
    }
    auto role = ReadRoleKey(rk);
    std::lock_guard<std::mutex> guard(roleMutex_);
    memberRoles_[rk] = role;
    poolEmptySinceUs_.store(0);
}

void SmemRallocEntry::EvaluatePoolEmpty()
{
    if (options_.role != SMEM_RALLOC_ROLE_FAR || !inited_ || globalGroup_ == nullptr) {
        return;
    }
    std::vector<uint32_t> ranks;
    globalGroup_->GetMemberRanks(ranks);
    std::lock_guard<std::mutex> guard(roleMutex_);
    for (auto r : ranks) {
        if (r == options_.rank) {
            continue;
        }
        auto it = memberRoles_.find(r);
        if (it == memberRoles_.end() || it->second != SMEM_RALLOC_ROLE_FAR) {
            return; /* a live NEAR member (or unknown role) is still around */
        }
    }
    if (poolEmptySinceUs_.load() == 0) {
        poolEmptySinceUs_.store(mf::MonotonicTime::TimeUs());
        SM_LOG_INFO("only FAR members left in pool, teardown pending, id: " << options_.id);
    }
}

bool SmemRallocEntry::IsPoolEmptyExpired(uint64_t graceSec) const
{
    auto marked = poolEmptySinceUs_.load();
    if (marked == 0) {
        return false;
    }
    return (mf::MonotonicTime::TimeUs() - marked) >= graceSec * 1000000ULL;
}

Result SmemRallocEntry::Join(uint32_t flags)
{
    SM_ASSERT_RETURN(inited_, SM_NOT_INITIALIZED);
    /* publish own role before joining so peers can read it when the join event arrives */
    SM_LOG_ERROR_RETURN_IT_IF_NOT_OK(WriteSelfRoleKey(), "write self role key failed, rank: " << options_.rank);
    const uint32_t groupJoinTimeoutSec =
        mf::MfEnvUtil::GetOptionalUintOrDefault("MF_GROUP_JOIN_MAX_TIMEOUT", MF_GROUP_JOIN_DEFAULT_TIMEOUT);
    SM_LOG_DEBUG("group join timeout sec: " << groupJoinTimeoutSec);
    auto start_time = std::chrono::steady_clock::now();
    while (true) {
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
        if (duration >= groupJoinTimeoutSec) {
            SM_LOG_ERROR("join timeout. rank: " << options_.rank << ", elapsed: " << duration << "s");
            return SM_ERROR;
        }
        auto ret = globalGroup_->GroupJoin();
        if (ret == SM_INNER_BUSY) {
            sleep(1U);
            continue;
        }
        SM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "join failed, ret: " << ret);
        SM_LOG_DEBUG("join success. rank: " << options_.rank);
        return SM_OK;
    }
}

Result SmemRallocEntry::ExtendLocalMem(smem_ralloc_mem_type_t memType, uint64_t size, smem_ralloc_mem_info_t *info)
{
    SM_ASSERT_RETURN(inited_, SM_NOT_INITIALIZED);
    SM_ASSERT_RETURN(memType == SMEM_RALLOC_MEM_TYPE_HOST || memType == SMEM_RALLOC_MEM_TYPE_DEVICE,
        SM_NOT_SUPPORTED);
    SM_ASSERT_RETURN(size > 0, SM_INVALID_PARAM);
    SM_ASSERT_RETURN(size % SMEM_RALLOC_SIZE_ALIGNMENT == 0, SM_INVALID_PARAM);
    const bool deviceMedia = memType == SMEM_RALLOC_MEM_TYPE_DEVICE;
    SM_ASSERT_RETURN(deviceMedia ? (coreOptions_.maxHBMSize > 0) : (coreOptions_.maxDRAMSize > 0),
        "extend on a media whose window is not reserved by create", SM_NOT_SUPPORTED);
    const auto hybmMemType = deviceMedia ? HYBM_MEM_TYPE_DEVICE : HYBM_MEM_TYPE_HOST;
    std::lock_guard<std::mutex> lock(mutex_);
    // 1.alloc slice
    auto slice = hybm_alloc_local_memory(entity_, hybmMemType, size, 0);
    if (slice == nullptr) {
        SM_LOG_ERROR("Failed to alloc memory, memType:" << (deviceMedia ? "device" : "host") << " size:" << size);
        return SM_ERROR;
    }
    // 2.export slice
    hybm_exchange_info sliceInfo{};
    auto ret = hybm_export(entity_, slice, 0, &sliceInfo);
    if (ret != 0) {
        SM_LOG_ERROR("Failed to export slice:" << slice << " memType:" << (deviceMedia ? "device" : "host")
                                               << " size:" << size);
        hybm_free_local_memory(entity_, slice, 1, 0);
        return ret;
    }
    slices_.push_back(slice);
    sliceInfos_.push_back(sliceInfo);
    // 3.group update
    for (uint32_t i = 0; i < SMEM_GROUP_RETRY_TIME; i++) {
        auto updateRet = globalGroup_->GroupUpdate();
        if (updateRet == SM_INNER_BUSY) {
            sleep(1U); // sleep 1s
            continue;
        }
        SM_LOG_ERROR_RETURN_IT_IF_NOT_OK(updateRet, "update failed, ret: " << updateRet);
        SM_LOG_DEBUG("update success. rank:" << options_.rank);
        // 4.fill info with the new block (mutex_ already held, do not call other locking methods)
        auto newGva = hybm_get_slice_va(entity_, slice);
        if (newGva == nullptr) {
            SM_LOG_ERROR("Failed to get slice va, slice:" << slice);
            return SM_ERROR;
        }
        if (deviceMedia) {
            deviceCommittedBytes_.fetch_add(size);
        } else {
            committedBytes_.fetch_add(size);
        }
        if (info != nullptr) {
            info->rankId = options_.rank;
            info->gva = newGva;
        }
        SM_LOG_INFO("extend local mem ok, rank: " << options_.rank << " memType: " << memType
                                                  << " gva: " << newGva << " size: " << size);
        return SM_OK;
    }
    SM_LOG_ERROR("group update timeout. rank:" << options_.rank);
    slices_.pop_back();
    sliceInfos_.pop_back();
    hybm_free_local_memory(entity_, slice, 1, 0);
    return SM_ERROR;
}

Result SmemRallocEntry::GetLocalMemInfo(smem_ralloc_mem_info_t *info)
{
    if (info == nullptr) {
        return SM_OK;
    }
    info->rankId = SMEM_RALLOC_INVALID_RANK;
    info->gva = nullptr;
    SM_ASSERT_RETURN(inited_, SM_NOT_INITIALIZED);
    std::lock_guard<std::mutex> lock(mutex_);
    if (slices_.empty()) {
        return SM_OK;
    }
    /* first local block = own slot base, not the window base (rank0 slot base) */
    auto gva = hybm_get_slice_va(entity_, slices_[0]);
    if (gva == nullptr) {
        SM_LOG_ERROR("Failed to get first slice va, slice:" << slices_[0]);
        return SM_ERROR;
    }
    info->rankId = options_.rank;
    info->gva = gva;
    return SM_OK;
}

uint64_t SmemRallocEntry::GetMemSizeByRank(uint32_t rank, smem_ralloc_mem_type_t memType)
{
    SM_ASSERT_RETURN(inited_, 0);
    const bool deviceMedia = memType == SMEM_RALLOC_MEM_TYPE_DEVICE;
    auto base = reinterpret_cast<uint64_t>(deviceMedia ? deviceGva_ : hostGva_);
    auto slotSize = deviceMedia ? coreOptions_.maxHBMSize : coreOptions_.maxDRAMSize;
    if (rank >= coreOptions_.rankCount || base == 0 || slotSize == 0) {
        return 0;
    }
    /* the window base is the rank0 slot base, identical in every process view, slot of rank r
     * spans [base + r * slotSize, +slotSize), same as GetMemPtrByRank */
    auto slotBase = base + static_cast<uint64_t>(rank) * slotSize;
    auto slotEnd = slotBase + slotSize;

    uint32_t count = 0;
    std::vector<hybm_va_range> ranges;
    for (uint32_t attempt = 0; attempt < 3U; attempt++) {
        auto queryRet = hybm_query_alloc_ranges(entity_, slotBase, slotEnd, ranges.data(), &count);
        if (queryRet == BM_OK) {
            break;
        }
        if (queryRet != BM_BUFFER_TOO_SMALL) {
            SM_LOG_ERROR("query alloc ranges failed, ret: " << queryRet);
            return 0;
        }
        /* count may grow again during concurrent extend, retry with the new capacity */
        ranges.assign(count, hybm_va_range{});
    }
    if (ranges.size() < count) {
        SM_LOG_ERROR("query alloc ranges keeps growing, give up this snapshot");
        return 0;
    }

    uint64_t extent = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (ranges[i].ownerRank == rank && ranges[i].gva >= slotBase) {
            extent = std::max(extent, ranges[i].gva + ranges[i].size - slotBase);
        }
    }
    return extent;
}

void *SmemRallocEntry::GetMemPtrByRank(uint32_t rank, smem_ralloc_mem_type_t memType)
{
    const bool deviceMedia = memType == SMEM_RALLOC_MEM_TYPE_DEVICE;
    auto base = deviceMedia ? deviceGva_ : hostGva_;
    auto slotSize = deviceMedia ? coreOptions_.maxHBMSize : coreOptions_.maxDRAMSize;
    if (!inited_ || base == nullptr || slotSize == 0 || rank >= coreOptions_.rankCount) {
        return nullptr;
    }
    return static_cast<char *>(base) + static_cast<uint64_t>(rank) * slotSize;
}

std::vector<uint32_t> SmemRallocEntry::GetGroupRanks()
{
    std::vector<uint32_t> ranks;
    if (!inited_ || globalGroup_ == nullptr) {
        return ranks;
    }
    globalGroup_->GetMemberRanks(ranks);
    return ranks;
}

Result SmemRallocEntry::DataCopy(const void *src, void *dest, uint64_t size, uint32_t flags)
{
    SM_VALIDATE_RETURN(src != nullptr, "invalid param, src is NULL", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(dest != nullptr, "invalid param, dest is NULL", SM_INVALID_PARAM);
    SM_VALIDATE_RETURN(size != 0, "invalid param, size is 0", SM_INVALID_PARAM);
    SM_ASSERT_RETURN(inited_, SM_NOT_INITIALIZED);
    SM_RETURN_IT_IF_NOT_OK(CheckJoined());

    hybm_copy_params copyParams = {const_cast<void *>(src), dest, size};
    auto ret = hybm_data_copy(entity_, &copyParams, HYBM_DATA_COPY_DIRECTION_AUTO, nullptr, flags);
    return ret == BM_NOT_CONNECTED ? SMEM_NOT_CONNECTED : ret;
}

Result SmemRallocEntry::Wait()
{
    SM_ASSERT_RETURN(inited_, SM_NOT_INITIALIZED);
    return hybm_wait(entity_);
}

Result SmemRallocEntry::SetGroupEventHandler(smem_ralloc_group_event_cb cb, void *context)
{
    SM_ASSERT_RETURN(cb != nullptr, SM_INVALID_PARAM);
    SM_ASSERT_RETURN(inited_, SM_NOT_INITIALIZED);
    std::unique_lock<std::mutex> locker{eventCbMutex_};
    eventCb_ = cb;
    eventCbCtx_ = context;
    return SM_OK;
}

uint32_t SmemRallocEntry::GetRankIdByGva(void *gva)
{
    if (AddrInHostGva(gva, 1UL)) {
        return ((uint64_t)gva - (uint64_t)hostGva_) / coreOptions_.maxDRAMSize;
    }
    if (AddrInDeviceGva(gva, 1UL)) {
        return ((uint64_t)gva - (uint64_t)deviceGva_) / coreOptions_.maxHBMSize;
    }
    return UINT32_MAX;
}

Result SmemRallocEntry::CheckJoined() const
{
    SM_VALIDATE_RETURN(globalGroup_ != nullptr && globalGroup_->IsJoined(), "not joined the net group yet",
                       SM_NOT_STARTED);
    return SM_OK;
}

Result SmemRallocEntry::CreateGlobalTeam(uint32_t rankSize, uint32_t rankId)
{
    SmemGroupChangeCallback joinFunc = std::bind(&SmemRallocEntry::JoinHandle, this, std::placeholders::_1);
    SmemGroupChangeCallback updateFunc = std::bind(&SmemRallocEntry::UpdateHandle, this, std::placeholders::_1);
    SmemGroupChangeCallback leaveFunc = std::bind(&SmemRallocEntry::LeaveHandle, this, std::placeholders::_1);
    SmemGroupOption opt = {rankSize,   rankId,   options_.controlOperationTimeout * SECOND_TO_MILLSEC, true, joinFunc,
                           updateFunc, leaveFunc};
    SmemGroupEnginePtr group = SmemNetGroupEngine::Create(configStore_, opt);
    SM_ASSERT_RETURN(group != nullptr, SM_ERROR);

    globalGroup_ = group;
    return SM_OK;
}

bool SmemRallocEntry::AddrInHostGva(const void *address, uint64_t size)
{
    if (hostGva_ == nullptr) {
        return false;
    }

    auto totalSize = coreOptions_.maxDRAMSize * coreOptions_.rankCount;
    if ((const uint8_t *)address + size > (const uint8_t *)hostGva_ + totalSize) {
        return false;
    }

    if ((const uint8_t *)address < (const uint8_t *)hostGva_) {
        return false;
    }

    return true;
}
bool SmemRallocEntry::AddrInDeviceGva(const void *address, uint64_t size)
{
    if (deviceGva_ == nullptr) {
        return false;
    }

    auto totalSize = coreOptions_.maxHBMSize * coreOptions_.rankCount;
    if ((const uint8_t *)address + size > (const uint8_t *)deviceGva_ + totalSize) {
        return false;
    }

    if ((const uint8_t *)address < (const uint8_t *)deviceGva_) {
        return false;
    }

    return true;
}
} // namespace smem
} // namespace ock
