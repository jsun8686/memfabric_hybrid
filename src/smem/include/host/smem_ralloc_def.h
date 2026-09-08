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
#ifndef __MEMFABRIC_SMEM_RALLOC_DEF_H__
#define __MEMFABRIC_SMEM_RALLOC_DEF_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *smem_ralloc_t;
#define SMEM_RALLOC_TIMEOUT_MAX UINT32_MAX /* all timeout must <= UINT32_MAX */
#define SMEM_RALLOC_TLS_PATH_SIZE 256
#define SMEM_RALLOC_INVALID_RANK UINT32_MAX /* sentinel of smem_ralloc_mem_info.rankId when no block is acquired */

/* ralloc control rpc port = rpcPortBase + rankId, keep away from store(8572)/hcom(10005+)/net(9980) */
#define SMEM_RALLOC_RPC_PORT_BASE_DEFAULT 11100U

/* all acquire sizes (window slot sizes and every extend size) must be aligned to 2M, same as hybm large page */
#define SMEM_RALLOC_SIZE_ALIGNMENT (2ULL * 1024ULL * 1024ULL)

/**
* @brief ralloc data operation type, bit values keep consistent with smem_bm_def.h,
* mapped to hybm layer bits by name via SmemRallocHelper::TransHybmDataOpType
*/
typedef enum {
    SMEMRA_DATA_OP_SDMA = 1U << 0,        /* data operation done by device SDMA */
    SMEMRA_DATA_OP_HOST_RDMA = 1U << 1,   /* data operation done by host RDMA */
    SMEMRA_DATA_OP_HOST_TCP = 1U << 2,    /* data operation done by host TCP */
    SMEMRA_DATA_OP_DEVICE_RDMA = 1U << 3, /* data operation done by device RDMA */
    SMEMRA_DATA_OP_HOST_URMA = 1U << 4,   /* data operation done by host URMA */
    SMEMRA_DATA_OP_HOST_SHM = 1U << 5,    /* same-node host shared memory (no network transport) */
    SMEMRA_DATA_OP_BUTT
} smem_ralloc_data_op_type;
typedef smem_ralloc_data_op_type smem_ralloc_data_op_type_t;

/**
 * @brief memory type, enum values keep consistent with smem_bm_def.h,
 * only SMEM_RALLOC_MEM_TYPE_HOST is supported by ralloc of current phase
 */
typedef enum {
    SMEM_RALLOC_MEM_TYPE_LOCAL_DEVICE = 0, /* memory on local device */
    SMEM_RALLOC_MEM_TYPE_LOCAL_HOST,       /* memory on local host */
    SMEM_RALLOC_MEM_TYPE_DEVICE,           /* memory on global device */
    SMEM_RALLOC_MEM_TYPE_HOST,             /* memory on global host */
    SMEM_RALLOC_MEM_TYPE_BUTT
} smem_ralloc_mem_type;
typedef smem_ralloc_mem_type smem_ralloc_mem_type_t;

/**
 * @brief node role of the ralloc deployment, NEAR nodes create pools and acquire remote
 * blocks, FAR nodes are resident contributors and the only placement candidates
 */
typedef enum {
    SMEM_RALLOC_ROLE_NEAR = 0, /* requester/accessor node, creates pools via smem_ralloc_create */
    SMEM_RALLOC_ROLE_FAR,      /* resident contributor node, contributes on JOIN_ALLOC requests */
    SMEM_RALLOC_ROLE_BUTT
} smem_ralloc_role;
typedef smem_ralloc_role smem_ralloc_role_t;

typedef struct {
    bool tlsEnable;
    char caPath[SMEM_RALLOC_TLS_PATH_SIZE];
    char crlPath[SMEM_RALLOC_TLS_PATH_SIZE];
    char certPath[SMEM_RALLOC_TLS_PATH_SIZE];
    char keyPath[SMEM_RALLOC_TLS_PATH_SIZE];
    char keyPassPath[SMEM_RALLOC_TLS_PATH_SIZE];
    char packagePath[SMEM_RALLOC_TLS_PATH_SIZE];
    char decrypterLibPath[SMEM_RALLOC_TLS_PATH_SIZE];
} smem_ralloc_tls_config;
typedef smem_ralloc_tls_config smem_ralloc_tls_config_t;

typedef struct {
    uint32_t initTimeout;             /* func smem_ralloc_init timeout, default 120s (min=1, max=SMEM_RALLOC_TIMEOUT_MAX) */
    uint32_t createTimeout;           /* func smem_ralloc_create timeout, default 120s (min=1, max=SMEM_RALLOC_TIMEOUT_MAX) */
    uint32_t controlOperationTimeout; /* control operation timeout, default 120s (min=1, max=SMEM_RALLOC_TIMEOUT_MAX) */
    bool startConfigStoreServer;      /* whether to start config store, default true */
    bool startConfigStoreOnly;        /* only start the config store */
    bool dynamicWorldSize;            /* member cannot join dynamically */
    bool unifiedAddressSpace;         /* unified address with SVM */
    bool autoRanking;                 /* automatically allocate rank IDs, default is true. */
    uint32_t rankId;                  /* user specified rank ID, valid for autoRanking is False */
    uint32_t flags;                   /* other flag, default 0 */
    smem_ralloc_role_t role;          /* node role, default SMEM_RALLOC_ROLE_FAR */
    uint16_t rpcPortBase;             /* control rpc port base, default SMEM_RALLOC_RPC_PORT_BASE_DEFAULT */
    char hcomUrl[64];
    smem_ralloc_tls_config hcomTlsConfig;
    smem_ralloc_tls_config storeTlsConfig;
    smem_ralloc_tls_config rpcTlsConfig;
} smem_ralloc_config_t;

typedef struct {
    uint64_t maxDramSize;                  /* the max size of one rank DRAM slot reserved in the window, must be 2M aligned */
    uint64_t maxHbmSize;                   /* reserved for the future HBM window slot, must be 2M aligned, device
                                              commit is not wired up in current phase */
    smem_ralloc_data_op_type dataOpType;   /* data operation type of the pool */
    bool enable56BitsGva;                  /* enable 56-bit GVA when total addr space exceeds 32TB */
    uint32_t flags;                        /* optional flags, default 0 */
} smem_ralloc_create_option_t;

typedef struct {
    uint32_t rankId; /* rank which contributes the memory of the acquired block,
                        SMEM_RALLOC_INVALID_RANK if no block is acquired */
    void *gva;       /* global virtual address of the acquired block, null if no block is acquired */
} smem_ralloc_mem_info;
typedef smem_ralloc_mem_info smem_ralloc_mem_info_t;

/**
 * @brief smem join/leave event type
 */
typedef enum {
    SMEM_RALLOC_GROUP_EVENT_JOIN,  /* join event */
    SMEM_RALLOC_GROUP_EVENT_LEAVE, /* leave event */
    SMEM_RALLOC_MEMBER_EVENT_BUTT
} smem_ralloc_group_event_t;

/**
 * @brief callback function for group member change event: join/leave,
 * @param handle           [in] ralloc object handle created by <i>smem_ralloc_create</i>
 * @param rankId           [in] rank ID
 * @param event            [in] event type <i>smem_ralloc_group_event_t</i>
 * @param context          [in] context passed in set_group_event_handler
 * @return void
 */
typedef void (*smem_ralloc_group_event_cb)(smem_ralloc_t handle, uint32_t rankId,
                                           smem_ralloc_group_event_t event, void *context);

#ifdef __cplusplus
}
#endif

#endif //__MEMFABRIC_SMEM_RALLOC_DEF_H__
