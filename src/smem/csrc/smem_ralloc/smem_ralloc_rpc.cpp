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
#include "smem_ralloc_rpc.h"

#include <chrono>
#include <cstring>
#include <vector>

#include "acc_tcp_shared_buf.h"
#include "mf_ipv4_validator.h"
#include "mf_tls_util.h"
#include "smem_logger.h"
#include "smem_ralloc_master.h"

namespace ock {
namespace smem {
SmemRallocRpcService &SmemRallocRpcService::Instance()
{
    static SmemRallocRpcService instance;
    return instance;
}

Result SmemRallocRpcService::Start(const SmemRallocRpcEndpoint &localEp, const smem_ralloc_tls_config &tlsConfig,
                                   uint32_t timeoutMs)
{
    std::lock_guard<std::mutex> guard(startMutex_);
    if (started_) {
        SM_LOG_WARN("smem ralloc rpc service already started");
        return SM_OK;
    }

    server_ = acc::AccTcpServer::Create();
    SM_ASSERT_RETURN(server_ != nullptr, SM_NEW_OBJECT_FAILED);

    localEp_ = localEp;
    tlsConfig_ = tlsConfig;
    timeoutMs_ = timeoutMs;

    if (tlsConfig_.tlsEnable && PrepareTls(server_) != SM_OK) {
        SM_LOG_ERROR("failed to prepare tls for ralloc rpc server");
        server_ = nullptr;
        return SM_ERROR;
    }

    if (mf::SocketAddressParserMgr::getInstance().CreateParser(
            "tcp://" + std::string(localEp_.ip) + ":" + std::to_string(localEp_.port)) == nullptr) {
        SM_LOG_ERROR("create socket parser failed for ralloc rpc port: " << localEp_.port);
        server_ = nullptr;
        return SM_ERROR;
    }

    server_->RegisterNewRequestHandler(
        SMEMRA_RPC_MSG_TYPE, [this](const acc::AccTcpRequestContext &ctx) { return OnRequest(ctx); });

    server_->RegisterNewLinkHandler(
        [](const acc::AccConnReq &req, const acc::AccTcpLinkComplexPtr &) {
            (void)req;
            return SM_OK;
        });

    server_->RegisterLinkBrokenHandler([](const acc::AccTcpLinkComplexPtr &link) {
        if (link != nullptr) {
            SM_LOG_INFO("ralloc rpc server link broken, linkId: " << link->Id());
        }
        return SM_OK;
    });

    acc::AccTcpServerOptions opt;
    opt.enableListener = true;
    opt.listenIp = localEp_.ip;
    opt.listenPort = localEp_.port;
    opt.workerCount = acc::UNO_2;
    opt.maxWorldSize = SMEM_WORLD_SIZE_MAX;

    auto ret = server_->Start(opt, TransTlsOption());
    if (ret != SM_OK) {
        SM_LOG_ERROR("start ralloc rpc server failed, result: " << ret << " port: " << localEp_.port);
        server_ = nullptr;
        return ret;
    }

    /* op handlers are registered on every node so that a future master failover never finds a
     * node without handlers; REGISTER/PLACEMENT are served only while the master service is
     * activated (IsRunning), PING always answers */
    RegisterHandler(SMEMRA_RPC_OP_PING, [](SmemRallocRpcMsg &m) {
        m.result = SM_OK;
        return SM_OK;
    });
    RegisterHandler(SMEMRA_RPC_OP_REGISTER, [](SmemRallocRpcMsg &m) -> Result {
        if (!SmemRallocMasterService::Instance().IsRunning()) {
            return SM_NOT_STARTED;
        }
        return SmemRallocMasterService::Instance().OnRegister(m);
    });
    RegisterHandler(SMEMRA_RPC_OP_PLACEMENT, [](SmemRallocRpcMsg &m) -> Result {
        if (!SmemRallocMasterService::Instance().IsRunning()) {
            return SM_NOT_STARTED;
        }
        return SmemRallocMasterService::Instance().OnPlacement(m);
    });

    started_ = true;
    SM_LOG_INFO("ralloc rpc server started, rank: " << localEp_.rankId << " endpoint: " << localEp_.ip << ":"
                                                     << localEp_.port);
    return SM_OK;
}

void SmemRallocRpcService::Stop()
{
    std::lock_guard<std::mutex> guard(startMutex_);
    if (!started_) {
        return;
    }

    {
        std::lock_guard<std::mutex> clientLocker(clientMutex_);
        for (auto &it : clients_) {
            if (it.second.client != nullptr) {
                it.second.client->Stop();
            }
        }
        clients_.clear();
    }
    FailPendingByKey("");

    if (server_ != nullptr) {
        server_->Stop();
        server_ = nullptr;
    }

    {
        std::lock_guard<std::mutex> handlerLocker(handlerMutex_);
        handlers_.clear();
    }

    started_ = false;
    SM_LOG_INFO("ralloc rpc service stopped");
}

bool SmemRallocRpcService::IsStarted() const
{
    return started_;
}

const SmemRallocRpcEndpoint &SmemRallocRpcService::GetLocalEndpoint() const
{
    return localEp_;
}

void SmemRallocRpcService::RegisterHandler(uint16_t op, const RpcHandler &h)
{
    std::lock_guard<std::mutex> guard(handlerMutex_);
    handlers_[op] = h;
}

Result SmemRallocRpcService::SyncCall(const SmemRallocRpcEndpoint &remote, SmemRallocRpcMsg &msg)
{
    SM_VALIDATE_RETURN(started_, "rpc service not started", SM_NOT_STARTED);
    SM_VALIDATE_RETURN(strlen(remote.ip) != 0, "remote ip is empty", SM_INVALID_PARAM);

    std::string key;
    auto ret = EnsureClient(remote, key);
    SM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "ensure rpc client failed, remote: " << remote.ip << ":" << remote.port);

    acc::AccTcpLinkComplexPtr link;
    {
        std::lock_guard<std::mutex> guard(clientMutex_);
        auto it = clients_.find(key);
        if (it == clients_.end() || !it->second.link->Established()) {
            SM_LOG_ERROR("rpc client link not available, remote: " << key);
            return SM_NOT_CONNECTED;
        }
        link = it->second.link;
    }

    msg.magic = SMEMRA_RPC_MAGIC;
    msg.msgVersion = SMEMRA_RPC_MSG_VERSION;
    msg.reqRank = localEp_.rankId;

    auto seqNo = seqGen_.fetch_add(1U);
    auto pending = std::make_shared<PendingCtx>();
    pending->clientKey = key;
    {
        std::lock_guard<std::mutex> guard(pendingMutex_);
        pending_.emplace(seqNo, pending);
    }

    auto dataBuf = acc::AccDataBuffer::Create(&msg, sizeof(SmemRallocRpcMsg));
    auto sendRet = link->NonBlockSend(SMEMRA_RPC_MSG_TYPE, seqNo, dataBuf, nullptr);
    if (sendRet != SM_OK) {
        {
            std::lock_guard<std::mutex> guard(pendingMutex_);
            pending_.erase(seqNo);
        }
        /* drop the broken client so next call reconnects */
        {
            std::lock_guard<std::mutex> clientGuard(clientMutex_);
            clients_.erase(key);
        }
        SM_LOG_ERROR("rpc send failed, result: " << sendRet << " remote: " << key);
        return SM_NOT_CONNECTED;
    }

    std::unique_lock<std::mutex> locker(pending->mtx);
    bool finished = pending->cond.wait_for(locker, std::chrono::milliseconds(timeoutMs_),
                                           [&pending]() { return pending->done; });
    if (!finished) {
        locker.unlock();
        std::lock_guard<std::mutex> guard(pendingMutex_);
        pending_.erase(seqNo);
        SM_LOG_ERROR("rpc call timeout, seqNo: " << seqNo << " remote: " << key);
        return SM_TIMEOUT;
    }
    auto resp = pending->resp;
    locker.unlock();

    std::lock_guard<std::mutex> guard(pendingMutex_);
    pending_.erase(seqNo);

    msg = resp;
    return SM_OK;
}

Result SmemRallocRpcService::EnsureClient(const SmemRallocRpcEndpoint &remote, std::string &key)
{
    key = std::string(remote.ip) + ":" + std::to_string(remote.port);
    std::lock_guard<std::mutex> guard(clientMutex_);
    auto it = clients_.find(key);
    if (it != clients_.end() && it->second.link != nullptr && it->second.link->Established()) {
        return SM_OK;
    }
    if (it != clients_.end()) {
        if (it->second.client != nullptr) {
            it->second.client->Stop();
        }
        clients_.erase(it);
    }

    if (mf::SocketAddressParserMgr::getInstance().CreateParser("tcp://" + key) == nullptr) {
        SM_LOG_ERROR("create socket parser failed for remote rpc: " << key);
        return SM_ERROR;
    }

    auto client = acc::AccTcpServer::Create();
    SM_ASSERT_RETURN(client != nullptr, SM_NEW_OBJECT_FAILED);
    if (tlsConfig_.tlsEnable && PrepareTls(client) != SM_OK) {
        SM_LOG_ERROR("failed to prepare tls for ralloc rpc client");
        return SM_ERROR;
    }
    client->RegisterNewRequestHandler(
        SMEMRA_RPC_MSG_TYPE, [this](const acc::AccTcpRequestContext &ctx) { return OnResponse(ctx); });

    client->RegisterLinkBrokenHandler([this, key](const acc::AccTcpLinkComplexPtr &l) {
        SM_LOG_WARN("rpc client link broken, remote: " << key << " linkId: " << l->Id());
        FailPendingByKey(key);
        std::lock_guard<std::mutex> guard(clientMutex_);
        clients_.erase(key);
        return SM_OK;
    });

    acc::AccTcpServerOptions opt; /* enableListener is false by default, client mode */
    opt.linkSendQueueSize = acc::UNO_48;
    auto ret = client->Start(opt, TransTlsOption());
    if (ret != SM_OK) {
        SM_LOG_ERROR("start ralloc rpc client failed, result: " << ret);
        return ret;
    }

    acc::AccConnReq connReq;
    connReq.reconnect = 0;
    connReq.rankId = localEp_.rankId;
    acc::AccTcpLinkComplexPtr link;
    ret = client->ConnectToPeerServer(remote.ip, remote.port, connReq, 3U, link);
    if (ret != SM_OK || link == nullptr) {
        SM_LOG_ERROR("connect to remote rpc failed, result: " << ret << " remote: " << key);
        client->Stop();
        return ret != SM_OK ? ret : SM_NOT_CONNECTED;
    }

    ClientEntry entry{client, link};
    clients_.emplace(key, entry);
    return SM_OK;
}

int32_t SmemRallocRpcService::OnRequest(const acc::AccTcpRequestContext &context)
{
    if (context.DataLen() != sizeof(SmemRallocRpcMsg)) {
        SM_LOG_ERROR("invalid rpc request len: " << context.DataLen());
        return SM_OK;
    }

    SmemRallocRpcMsg msg{};
    (void)memcpy(&msg, context.DataPtr(), sizeof(SmemRallocRpcMsg));
    if (msg.magic != SMEMRA_RPC_MAGIC) {
        SM_LOG_ERROR("invalid rpc request magic: " << msg.magic);
        return SM_OK;
    }
    if (msg.msgVersion != SMEMRA_RPC_MSG_VERSION) {
        SM_LOG_ERROR("invalid rpc request msgVersion: " << msg.msgVersion << " expect: " << SMEMRA_RPC_MSG_VERSION);
        return SM_OK;
    }

    RpcHandler handler;
    {
        std::lock_guard<std::mutex> guard(handlerMutex_);
        auto it = handlers_.find(msg.op);
        if (it != handlers_.end()) {
            handler = it->second;
        }
    }

    if (handler == nullptr) {
        SM_LOG_WARN("no handler for rpc op: " << msg.op);
        msg.result = SM_NOT_SUPPORTED;
    } else {
        msg.result = handler(msg);
        if (msg.result != SM_OK) {
            SM_LOG_WARN("rpc op: " << msg.op << " handled with result: " << msg.result);
        }
    }

    auto respBuf = acc::AccDataBuffer::Create(&msg, sizeof(SmemRallocRpcMsg));
    auto ret = context.Reply(SM_OK, respBuf);
    if (ret != SM_OK) {
        SM_LOG_ERROR("rpc reply failed, result: " << ret);
    }
    return SM_OK;
}

int32_t SmemRallocRpcService::OnResponse(const acc::AccTcpRequestContext &context)
{
    if (context.DataLen() != sizeof(SmemRallocRpcMsg)) {
        SM_LOG_ERROR("invalid rpc response len: " << context.DataLen());
        return SM_OK;
    }

    std::shared_ptr<PendingCtx> pending;
    {
        std::lock_guard<std::mutex> guard(pendingMutex_);
        auto it = pending_.find(context.SeqNo());
        if (it != pending_.end()) {
            pending = it->second;
        }
    }
    if (pending == nullptr) {
        SM_LOG_WARN("rpc response without pending ctx, seqNo: " << context.SeqNo());
        return SM_OK;
    }

    {
        std::lock_guard<std::mutex> locker(pending->mtx);
        (void)memcpy(&pending->resp, context.DataPtr(), sizeof(SmemRallocRpcMsg));
        pending->done = true;
    }
    pending->cond.notify_one();
    return SM_OK;
}

void SmemRallocRpcService::FailPendingByKey(const std::string &key)
{
    std::vector<std::shared_ptr<PendingCtx>> failList;
    {
        std::lock_guard<std::mutex> guard(pendingMutex_);
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (key.empty() || it->second->clientKey == key) {
                failList.push_back(it->second);
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto &ctx : failList) {
        {
            std::lock_guard<std::mutex> locker(ctx->mtx);
            ctx->resp.result = SM_NOT_CONNECTED;
            ctx->done = true;
        }
        ctx->cond.notify_one();
    }
}

Result SmemRallocRpcService::PrepareTls(const acc::AccTcpServerPtr &server) const
{
    if (server == nullptr) {
        return SM_ERROR;
    }
    if (server->LoadDynamicLib(tlsConfig_.packagePath) != 0) {
        SM_LOG_ERROR("failed to load openssl dynamic library");
        return SM_ERROR;
    }
    if (strlen(tlsConfig_.keyPassPath) == 0) {
        SM_LOG_WARN("no private key password provided, using unencrypted private key");
        return SM_OK;
    }
    if (strlen(tlsConfig_.decrypterLibPath) == 0) {
        SM_LOG_WARN("no decrypter provided, using default decrypter handler");
        server->RegisterDecryptHandler(mf::MfTlsUtil::DefaultDecrypter);
        return SM_OK;
    }
    const auto decrypter = mf::MfTlsUtil::LoadDecryptFunction(tlsConfig_.decrypterLibPath);
    if (decrypter == nullptr) {
        SM_LOG_ERROR("failed to load customized decrypt function");
        return SM_ERROR;
    }
    server->RegisterDecryptHandler(decrypter);
    return SM_OK;
}

acc::AccTlsOption SmemRallocRpcService::TransTlsOption() const
{
    acc::AccTlsOption option{};
    option.enableTls = tlsConfig_.tlsEnable;
    option.tlsTopPath = "/";
    option.tlsCaPath = "/";
    option.tlsCrlPath = "/";
    option.tlsCert = tlsConfig_.certPath;
    option.tlsPk = tlsConfig_.keyPath;
    option.tlsPkPwd = tlsConfig_.keyPassPath;
    std::string caFile = tlsConfig_.caPath;
    if (!caFile.empty()) {
        option.tlsCaFile.insert(caFile);
    }
    std::string crlFile = tlsConfig_.crlPath;
    if (!crlFile.empty()) {
        option.tlsCrlFile.insert(crlFile);
    }
    return option;
}
} // namespace smem
} // namespace ock
