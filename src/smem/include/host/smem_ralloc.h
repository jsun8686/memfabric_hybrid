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
#ifndef __MEMFABRIC_SMEM_RALLOC_H__
#define __MEMFABRIC_SMEM_RALLOC_H__

#include "smem.h"
#include "smem_ralloc_def.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ralloc role model (see smem_ralloc_config_t.role):
 * - NEAR: requester/accessor node, the only role allowed to call smem_ralloc_create and hold
 *         handles; acquires blocks via extend_local_mem/extend_remote_mem
 * - FAR:  resident contributor node, registers as placement candidate and contributes on
 *         JOIN_ALLOC requests; holds no application handle, its executor entries self-teardown
 *         after a grace period once no NEAR member is left in the pool
 */

/**
 * @brief Set the config to default values
 *
 * @param config           [in] the config to be set
 * @return 0 if successful
 */
int32_t smem_ralloc_config_init(smem_ralloc_config_t *config);

/**
 * @brief Initialize ralloc library, which acquires memory blocks from remote contributor
 * nodes into one global shared memory space. NEAR role nodes create pools and join the
 * dynamic group; FAR role contributor nodes join on demand when JOIN_ALLOC requests arrive.
 *
 * @param storeURL         [in] configure store url for control,
 *                              e.g. tcp://ip:port, tcp://[ip]:port, etcd://[ip]:port,
 *                              etcd://[ip]:port#instanceId, reg://[ip]:port,
 *                              or reg://[ip]:port#instanceId (for multi-cluster isolation)
 * @param worldSize        [in] max number of guys participating (window rank count)
 * @param deviceId         [in] device id
 * @param config           [in] extract config
 * @return 0 if successful
 */
int32_t smem_ralloc_init(const char *storeURL, uint32_t worldSize, uint16_t deviceId,
                         const smem_ralloc_config_t *config);

/**
 * @brief Un-initialize ralloc library with destroy all things
 *
 * @param flags            [in] optional flags, set to 0
 */
void smem_ralloc_uninit(uint32_t flags);

/**
 * @brief Get the rank id, assigned during initialization i.e. after call <i>smem_ralloc_init</i>
 *
 * @return rank id
 */
uint32_t smem_ralloc_get_rank_id(void);

/**
 * @brief Create ralloc object with specified id and join the dynamic group of the pool,
 * available to NEAR role nodes only (FAR nodes contribute via remote requests instead).
 * The first call of one id builds local entity with window reserved (rankCnt = worldSize),
 * no local memory is committed by create itself, use smem_ralloc_extend_* to acquire blocks.
 * Application must guarantee the first create of one pool is not concurrent with others.
 *
 * @param id               [in] ralloc object id, different pools need different ids
 * @param option           [in] create options, must be identical among all ranks of one pool
 * @return handle of ralloc object if successful, null if failed
 */
smem_ralloc_t smem_ralloc_create(uint32_t id, const smem_ralloc_create_option_t *option);

/**
 * @brief Destroy ralloc object, all local memory of the entry is released with it
 *
 * @param handle           [in] ralloc object handle created by <i>smem_ralloc_create</i>
 */
void smem_ralloc_destroy(smem_ralloc_t handle);

/**
 * @brief Copy data with direction automatically selected by address
 *
 * @param handle           [in] ralloc object handle created by <i>smem_ralloc_create</i>
 * @param src              [in] source address, local or global
 * @param dest             [in] destination address, local or global
 * @param size             [in] data size in byte
 * @param flags            [in] optional flags, e.g. ASYNC_COPY_FLAG
 * @return 0 if successful
 */
int32_t smem_ralloc_copy(smem_ralloc_t handle, const void *src, void *dest, uint64_t size, uint32_t flags);

/**
 * @brief Wait all asynchronous copy finished
 *
 * Applies to the SDMA asynchronous path only: HOST data paths (HOST_RDMA etc.) complete
 * synchronously inside <i>smem_ralloc_copy</i> and need no wait.
 *
 * @param handle           [in] ralloc object handle created by <i>smem_ralloc_create</i>
 * @return 0 if successful; an error when the entity has no SDMA data operator
 */
int32_t smem_ralloc_wait(smem_ralloc_t handle);

/**
 * @brief Extend one memory block on local slot, members of the pool import it via UPDATE event
 *
 * @param handle           [in] ralloc object handle created by <i>smem_ralloc_create</i>
 * @param memType          [in] memory type, SMEM_RALLOC_MEM_TYPE_HOST or SMEM_RALLOC_MEM_TYPE_DEVICE
 * @param size             [in] block size in byte, must be 2M aligned
 * @param info             [out] memory info of the new block, can be null if not care
 * @return 0 if successful
 */
int32_t smem_ralloc_extend_local_mem(smem_ralloc_t handle, smem_ralloc_mem_type_t memType, uint64_t size,
                                     smem_ralloc_mem_info_t *info);

/**
 * @brief Extend one memory block on a remote contributor node, which is selected by the master
 * of the deployment (least loaded candidate on the requested media, never the requester itself).
 * The selected node contributes the memory and joins the pool on demand.
 *
 * @param handle           [in] ralloc object handle created by <i>smem_ralloc_create</i>
 * @param memType          [in] memory type, SMEM_RALLOC_MEM_TYPE_HOST or SMEM_RALLOC_MEM_TYPE_DEVICE
 * @param size             [in] block size in byte, must be 2M aligned
 * @param info             [out] memory info of the new block, rankId is the contributor rank,
 *                              can be null if not care
 * @return 0 if successful
 */
int32_t smem_ralloc_extend_remote_mem(smem_ralloc_t handle, smem_ralloc_mem_type_t memType, uint64_t size,
                                      smem_ralloc_mem_info_t *info);

/**
 * @brief Get the current committed size of one rank's slot of the pool, which is a snapshot of
 * the local imported state: blocks committed by other ranks are visible after the corresponding
 * JOIN/UPDATE event has been processed by the local side.
 *
 * @param handle           [in] ralloc object handle created by <i>smem_ralloc_create</i>
 * @param rank             [in] rank id of the slot
 * @param memType          [in] memory type of the window the slot belongs to
 * @return committed size in byte, 0 if the rank is invalid or has no imported block
 */
uint64_t smem_ralloc_get_mem_size_by_rank(smem_ralloc_t handle, uint32_t rank,
                                          smem_ralloc_mem_type_t memType);

/**
 * @brief Get slot base address of one rank, paired with <i>smem_ralloc_get_mem_size_by_rank</i>
 * to make up the valid range of the slot. Valid range of one rank's slot is [ptr, ptr + size),
 * the rest of the slot is reserved but not committed.
 *
 * @param handle           [in] ralloc object handle created by <i>smem_ralloc_create</i>
 * @param rank             [in] rank ID of the slot
 * @param memType          [in] memory type of the window the slot belongs to
 * @return slot base address, null if failed
 */
void *smem_ralloc_get_mem_ptr_by_rank(smem_ralloc_t handle, uint32_t rank, smem_ralloc_mem_type_t memType);

/**
 * @brief Get the ranks currently in the pool's dynamic group, which is a snapshot taken at the
 * moment of the call and includes the local rank itself. The event stream is single-slot and can
 * not be replayed, so members joined before the handler registration are only discoverable by
 * this interface, use <i>smem_ralloc_set_group_event_handler</i> to track the increments after
 * registration. The caller is expected to size the buffer to the world size known from init.
 * Pair with <i>smem_ralloc_get_mem_size_by_rank</i>: a member rank with size 0 means it is in
 * the group but has no committed block.
 *
 * @param handle           [in] ralloc object handle created by <i>smem_ralloc_create</i>
 * @param rankIds          [in] buffer for rank ids, null with maxCount 0 makes a count-only call
 * @param maxCount         [in] buffer capacity in element, world size is always enough
 * @return actual member count, only min(count, maxCount) entries are written and ret > maxCount
 *         means the buffer is too small, UINT32_MAX if failed
 */
uint32_t smem_ralloc_get_group_ranks(smem_ralloc_t handle, uint32_t *rankIds, uint32_t maxCount);

/**
 * @brief Set event handler for group member change, join or leave.
 * Events fired after registration only: the underlying event stream is single-slot and can not
 * be replayed, members joined before the registration (including the ones joined before the
 * local rank) are not reported, query them by <i>smem_ralloc_get_group_ranks</i> instead.
 *
 * @param handle           [in] ralloc object handle created by <i>smem_ralloc_create</i>
 * @param cb               [in] callback function
 * @param context          [in] context passed to callback
 * @return 0 if successful
 */
int32_t smem_ralloc_set_group_event_handler(smem_ralloc_t handle, smem_ralloc_group_event_cb cb, void *context);

#ifdef __cplusplus
}
#endif

#endif //__MEMFABRIC_SMEM_RALLOC_H__
