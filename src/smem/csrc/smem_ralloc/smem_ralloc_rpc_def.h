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
#ifndef MEMFABRIC_HYBRID_SMEM_RALLOC_RPC_DEF_H
#define MEMFABRIC_HYBRID_SMEM_RALLOC_RPC_DEF_H

#include <cstdint>
#include <cassert>

#include "smem_types.h"

namespace ock {
namespace smem {
/* internal wire format of the ralloc control plane, carried by acc_tcp, both directions use
 * the same fixed-size POD struct, fields marked [resp] are meaningful in response only,
 * fields marked [register] are meaningful in SMEMRA_RPC_OP_REGISTER request only. */
constexpr uint16_t SMEMRA_RPC_MAGIC = 0x524AU;      /* 'R','A' */
constexpr uint16_t SMEMRA_RPC_MSG_VERSION = 1U;
constexpr int16_t SMEMRA_RPC_MSG_TYPE = 1; /* acc_tcp route tag, valid range [MIN_MSG_TYPE, MAX_MSG_TYPE) = [0,48), 0 is taken by the tcp store */
constexpr uint32_t SMEMRA_RPC_MASTER_KEY_MAX_LEN = 46U;

enum SmemRallocRpcOp : uint16_t {
    SMEMRA_RPC_OP_REGISTER = 1,    /* node -> master: register as placement candidate, carries committed bytes */
    SMEMRA_RPC_OP_PLACEMENT = 2,   /* requester -> master: pick one node for a block */
    SMEMRA_RPC_OP_JOIN_ALLOC = 3,  /* requester -> contributor: contribute and join pool, return gva */
    SMEMRA_RPC_OP_PING = 5,        /* health check */
    SMEMRA_RPC_OP_BUTT
};

struct SmemRallocRpcEndpoint {
    uint32_t rankId;
    uint16_t port;
    uint16_t reserved;
    char ip[SMEMRA_RPC_MASTER_KEY_MAX_LEN]; /* null-terminated, ipv4/ipv6 string */
};

struct SmemRallocRpcMsg {
    uint16_t op;
    uint16_t magic;
    uint16_t msgVersion;
    uint16_t enable56BitsGva;
    uint32_t poolId;
    uint32_t reqRank;      /* rank of the requester */
    uint32_t ownerRank;    /* [resp] rank of the contributor node */
    uint32_t dataOpType;   /* smem_ralloc_data_op_type bits */
    uint32_t flags;
    uint32_t memType;      /* smem_ralloc_mem_type of the requested block, HOST only in current phase */
    uint32_t result;       /* [resp] SM_* result code */
    uint32_t nodeRank;     /* [resp][register] endpoint rank */
    uint32_t nodePort;     /* [resp][register] endpoint port */
    uint32_t reserved1;
    uint64_t size;         /* requested region size in byte, reused as the committed bytes report in REGISTER */
    uint64_t maxDramSize;  /* window slot size of the pool */
    uint64_t gva;          /* [resp] global virtual address of the region */
    char nodeIp[SMEMRA_RPC_MASTER_KEY_MAX_LEN]; /* [resp][register] endpoint ip */
    uint8_t pad[10];
};
static_assert(sizeof(SmemRallocRpcMsg) == 128U, "smem ralloc rpc msg size must be 128");
static_assert(sizeof(SmemRallocRpcEndpoint) == 56U, "smem ralloc rpc endpoint size must be 56 (54 bytes + 2 tail padding)");

/* master endpoint published under the RA_ prefixed store, value is POD SmemRallocRpcEndpoint */
constexpr const char *SMEMRA_RPC_MASTER_STORE_KEY = "MASTER";

/* pool scoped store key carrying the node role, key = prefix + rankId, value is uint32_t role;
 * written before joining the group so peers can read it when the join event arrives */
constexpr const char *SMEMRA_POOL_ROLE_KEY_PREFIX = "RA_ROLE_";
constexpr uint32_t SMEMRA_ROLE_KEY_READ_TIMEOUT_MS = 1000U;
} // namespace smem
} // namespace ock

#endif // MEMFABRIC_HYBRID_SMEM_RALLOC_RPC_DEF_H
