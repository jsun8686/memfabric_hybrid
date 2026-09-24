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

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <memory>
#include <thread>

#include <gtest/gtest.h>

#include "smem_ralloc_entry.h"
#include "smem_ralloc_master.h"
#include "smem_ralloc_rpc_def.h"

using namespace ock::smem;

namespace {
constexpr uint64_t K_GB = 1024ULL * 1024ULL * 1024ULL;
constexpr uint32_t K_REQ_RANK = 9U; /* requester rank, never a candidate below */

SmemRallocRpcMsg MakeRegisterMsg(uint32_t rank, uint64_t hostBytes, uint64_t deviceBytes)
{
    SmemRallocRpcMsg msg{};
    msg.op = SMEMRA_RPC_OP_REGISTER;
    msg.nodeRank = rank;
    msg.nodePort = 11100U + rank;
    (void)snprintf(msg.nodeIp, sizeof(msg.nodeIp), "10.90.22.35");
    msg.size = hostBytes;
    msg.deviceCommittedBytes = deviceBytes;
    return msg;
}

SmemRallocRpcMsg MakeDevicePlacementMsg(uint64_t size)
{
    SmemRallocRpcMsg msg{};
    msg.op = SMEMRA_RPC_OP_PLACEMENT;
    msg.reqRank = K_REQ_RANK;
    msg.size = size;
    msg.memType = static_cast<uint32_t>(SMEM_RALLOC_MEM_TYPE_DEVICE);
    /* window 0 disables the master capacity filter: keep the test on pure load order */
    msg.maxDramSize = 0U;
    msg.maxHbmSize = 0U;
    return msg;
}

SmemRallocRpcMsg MakeGrantFailMsg(uint32_t nodeRank, uint64_t size)
{
    SmemRallocRpcMsg msg{};
    msg.op = SMEMRA_RPC_OP_GRANT_FAIL;
    msg.nodeRank = nodeRank;
    msg.size = size;
    msg.memType = static_cast<uint32_t>(SMEM_RALLOC_MEM_TYPE_DEVICE);
    return msg;
}
} // namespace

/* ---------------- master: GRANT_FAIL releases the optimistic reservation ---------------- */

TEST(SmemRallocMasterLbTest, GrantFailReleasesInflightReservation)
{
    SmemRallocMasterService master;
    ASSERT_EQ(master.OnRegister(MakeRegisterMsg(1U, 0U, 50 * K_GB)), SM_OK);
    ASSERT_EQ(master.OnRegister(MakeRegisterMsg(2U, 0U, 0U)), SM_OK);

    auto place = MakeDevicePlacementMsg(100 * K_GB);

    /* least device-media load: rank 2 (0 GB) over rank 1 (50 GB) */
    ASSERT_EQ(master.OnPlacement(place), SM_OK);
    ASSERT_EQ(place.nodeRank, 2U);

    /* rank 2 now carries a 100 GB in-flight grant, rank 1 (50 GB) is lighter */
    ASSERT_EQ(master.OnPlacement(place), SM_OK);
    ASSERT_EQ(place.nodeRank, 1U);

    /* the rank 1 grant failed to land on the executor: NACK releases it */
    ASSERT_EQ(master.OnGrantFail(MakeGrantFailMsg(1U, 100 * K_GB)), SM_OK);

    /* without the release rank 1 would still show 150 GB and lose to rank 2 (100 GB) */
    ASSERT_EQ(master.OnPlacement(place), SM_OK);
    ASSERT_EQ(place.nodeRank, 1U);
}

TEST(SmemRallocMasterLbTest, GrantFailUnknownNodeOrNoMatchIsNoop)
{
    SmemRallocMasterService master;

    /* no candidate table at all: unknown node is a benign no-op */
    ASSERT_EQ(master.OnGrantFail(MakeGrantFailMsg(7U, 100 * K_GB)), SM_OK);

    ASSERT_EQ(master.OnRegister(MakeRegisterMsg(1U, 0U, 0U)), SM_OK);
    /* size matches no in-flight grant: nothing released, still a no-op */
    ASSERT_EQ(master.OnGrantFail(MakeGrantFailMsg(1U, 100 * K_GB)), SM_OK);

    auto place = MakeDevicePlacementMsg(100 * K_GB);
    ASSERT_EQ(master.OnPlacement(place), SM_OK);
    ASSERT_EQ(place.nodeRank, 1U);
}

TEST(SmemRallocMasterLbTest, GrantFailRejectsInvalidParams)
{
    SmemRallocMasterService master;

    auto fail = MakeGrantFailMsg(1U, 0U);
    ASSERT_EQ(master.OnGrantFail(fail), SM_INVALID_PARAM);

    fail.size = 100 * K_GB;
    fail.memType = 99U;
    ASSERT_EQ(master.OnGrantFail(fail), SM_INVALID_PARAM);
}

/* ---------------- entry: extend parking during the first-JOIN_ALLOC bootstrap ---------------- */

class SmemRallocEntryBootstrapTest : public testing::Test {
protected:
    void SetUp() override
    {
        SmemRallocEntryOptions opts{};
        opts.id = 1U;
        opts.rank = 0U;
        opts.rankSize = 2U;
        opts.controlOperationTimeout = 1000U;
        opts.role = SMEM_RALLOC_ROLE_FAR;
        entry_ = std::make_unique<SmemRallocEntry>(opts, StorePtr{});
    }

    std::unique_ptr<SmemRallocEntry> entry_;
};

TEST_F(SmemRallocEntryBootstrapTest, NoBootstrapReturnsOkImmediately)
{
    auto begin = std::chrono::steady_clock::now();
    ASSERT_EQ(entry_->WaitForBootstrap(), SM_OK);
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - begin).count();
    ASSERT_LT(elapsedMs, 1000);
}

TEST_F(SmemRallocEntryBootstrapTest, ParksUntilBootstrapSucceeds)
{
    entry_->MarkBootstrapRunning();
    std::thread finisher([this]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        entry_->MarkBootstrapDone(true);
    });
    auto begin = std::chrono::steady_clock::now();
    ASSERT_EQ(entry_->WaitForBootstrap(), SM_OK);
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - begin).count();
    ASSERT_GE(elapsedMs, 100); /* really parked behind the bootstrap */
    finisher.join();

    /* READY is sticky: later waiters pass without parking */
    ASSERT_EQ(entry_->WaitForBootstrap(), SM_OK);
}

TEST_F(SmemRallocEntryBootstrapTest, BootstrapFailurePropagatesToWaiters)
{
    entry_->MarkBootstrapRunning();
    std::thread finisher([this]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        entry_->MarkBootstrapDone(false);
    });
    ASSERT_EQ(entry_->WaitForBootstrap(), SM_NOT_INITIALIZED);
    finisher.join();

    /* FAILED is sticky and fails fast afterwards */
    ASSERT_EQ(entry_->WaitForBootstrap(), SM_NOT_INITIALIZED);
}

TEST_F(SmemRallocEntryBootstrapTest, BootstrapTimeoutFailsClosed)
{
    ASSERT_EQ(setenv("MF_RALLOC_BOOTSTRAP_WAIT_SEC", "1", 1), 0);
    entry_->MarkBootstrapRunning();
    auto begin = std::chrono::steady_clock::now();
    ASSERT_EQ(entry_->WaitForBootstrap(), SM_NOT_INITIALIZED);
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - begin).count();
    ASSERT_GE(elapsedMs, 900);
    unsetenv("MF_RALLOC_BOOTSTRAP_WAIT_SEC");
}
