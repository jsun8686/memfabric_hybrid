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
#ifndef MEMFABRIC_HYBRID_SMEM_RALLOC_RPC_H
#define MEMFABRIC_HYBRID_SMEM_RALLOC_RPC_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "acc_def.h"
#include "acc_tcp_server.h"
#include "smem_ralloc.h"
#include "smem_ralloc_rpc_def.h"
#include "smem_timedwait.h"

namespace ock {
namespace smem {
/*
 * Process level singleton rpc service of ralloc:
 * - one acc_tcp server with listener enabled, port = rpcPortBase + rankId
 * - sync client with cached connections, keyed by "ip:port"
 * - op dispatch: master service / x executor register handlers by op code
 */
class SmemRallocRpcService {
public:
    using RpcHandler = std::function<Result(SmemRallocRpcMsg &)>;

    static SmemRallocRpcService &Instance();

    SmemRallocRpcService() = default;
    ~SmemRallocRpcService() = default;

    SmemRallocRpcService(const SmemRallocRpcService &) = delete;
    SmemRallocRpcService(SmemRallocRpcService &&) = delete;
    SmemRallocRpcService &operator=(const SmemRallocRpcService &other) = delete;
    SmemRallocRpcService &operator=(SmemRallocRpcService &&other) = delete;

    Result Start(const SmemRallocRpcEndpoint &localEp, const smem_ralloc_tls_config &tlsConfig, uint32_t timeoutMs);

    void Stop();

    bool IsStarted() const;

    const SmemRallocRpcEndpoint &GetLocalEndpoint() const;

    /* send one request to remote endpoint and wait for response, msg.result carries remote result */
    Result SyncCall(const SmemRallocRpcEndpoint &remote, SmemRallocRpcMsg &msg);

    void RegisterHandler(uint16_t op, const RpcHandler &h);

private:
    struct ClientEntry {
        acc::AccTcpServerPtr client;
        acc::AccTcpLinkComplexPtr link;
    };

    struct PendingCtx {
        std::mutex mtx;
        std::condition_variable cond;
        bool done = false;
        SmemRallocRpcMsg resp{};
        std::string clientKey;
    };

    Result EnsureClient(const SmemRallocRpcEndpoint &remote, std::string &key);

    int32_t OnRequest(const acc::AccTcpRequestContext &context);

    int32_t OnResponse(const acc::AccTcpRequestContext &context);

    void FailPendingByKey(const std::string &key);

    Result PrepareTls(const acc::AccTcpServerPtr &server) const;

    acc::AccTlsOption TransTlsOption() const;

    std::mutex startMutex_;
    std::atomic<bool> started_{false};
    SmemRallocRpcEndpoint localEp_{};
    smem_ralloc_tls_config tlsConfig_{};
    uint32_t timeoutMs_ = SMEM_DEFAUT_WAIT_TIME * SECOND_TO_MILLSEC;

    acc::AccTcpServerPtr server_;

    std::mutex clientMutex_;
    std::map<std::string, ClientEntry> clients_;

    std::atomic<uint32_t> seqGen_{0};
    std::mutex pendingMutex_;
    std::unordered_map<uint32_t, std::shared_ptr<PendingCtx>> pending_;

    std::mutex handlerMutex_;
    std::map<uint16_t, RpcHandler> handlers_;
};
} // namespace smem
} // namespace ock

#endif // MEMFABRIC_HYBRID_SMEM_RALLOC_RPC_H
