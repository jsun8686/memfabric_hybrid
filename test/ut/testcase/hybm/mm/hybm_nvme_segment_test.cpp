/*
 * Copyright: (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
*/

#include <gtest/gtest.h>
#include <mockcpp/mockcpp.hpp>

#define private public
#define protected public
#include "hybm_nvme_segment.h"
#undef private
#undef protected

#include "hybm_mem_slice.h"
#include "hybm_va_manager.h"

using namespace ock::mf;

namespace {
/* in-memory admin client: records add/remove calls, no SPDK dependency */
class MockNvmeAdmin : public HybmNvmeAdminClient {
public:
    /* fully qualified return types: mockcpp.hpp injects a global mockcpp::Result that is
     * ambiguous with ock::mf::Result under `using namespace ock::mf` */
    ock::mf::Result AddNs(uint64_t lbaOffset, uint64_t lbaLen, uint32_t &nsid) override
    {
        added.emplace_back(lbaOffset, lbaLen);
        nsid = ++nextNsid;
        return BM_OK;
    }

    ock::mf::Result RemoveNs(uint32_t nsid) override
    {
        removed.push_back(nsid);
        return BM_OK;
    }

    bool Healthy() override
    {
        return healthy;
    }

    std::vector<std::pair<uint64_t, uint64_t>> added;
    std::vector<uint32_t> removed;
    uint32_t nextNsid = 0;
    bool healthy = true;
};

MemSegmentOptions OwnerOptions()
{
    MemSegmentOptions opt{};
    opt.segType = HYBM_MST_NVME;
    opt.maxSize = 4ULL * 1024ULL * 1024ULL * 1024ULL; /* 4G window */
    opt.rankCnt = 2UL;
    opt.rankId = 0;
    return opt;
}
} // namespace

class HybmNvmeSegmentTest : public testing::Test {
protected:
    void SetUp() override
    {
        GlobalMockObject::reset();
        HybmVaManager::GetInstance().ClearAll();
        HybmNvmeExtentRegistry::GetInstance().Clear();
    }

    void TearDown() override
    {
        GlobalMockObject::verify();
        GlobalMockObject::reset();
        HybmVaManager::GetInstance().ClearAll();
        HybmNvmeExtentRegistry::GetInstance().Clear();
    }
};

/**
 * ReserveAndAlloc_ExposesExtentAndRegisters
 *  - 预留带后分配 extent：VA/registry/导出信息三者一致，管理粒度对齐。
 */
TEST_F(HybmNvmeSegmentTest, ReserveAndAlloc_ExposesExtentAndRegisters)
{
    auto admin = std::make_shared<MockNvmeAdmin>();
    HybmNvmeSegment seg(OwnerOptions(), 0, admin);

    void *band = nullptr;
    ASSERT_EQ(seg.ReserveMemorySpace(&band), BM_OK);
    ASSERT_NE(band, nullptr);

    const uint64_t size = 2ULL * 1024 * 1024; /* 2M */
    MemSlicePtr slice;
    ASSERT_EQ(seg.AllocLocalMemory(size, slice), BM_OK);
    ASSERT_NE(slice, nullptr);

    /* LBA carving is granularity-aligned (default 1G): one whole split run */
    ASSERT_EQ(admin->added.size(), 1U);
    ASSERT_EQ(admin->added[0].second % (1ULL << 30), 0U);

    /* registry resolves the extent with the nvmf identity tail */
    HybmNvmeExtent extent;
    ASSERT_TRUE(HybmNvmeExtentRegistry::GetInstance().Lookup(slice->gva_, size, extent));
    ASSERT_EQ(extent.ownerRank, 0U);
    ASSERT_EQ(extent.nsid, 1U);
    ASSERT_EQ(extent.lbaLen * 512ULL, admin->added[0].second);

    /* export info is the unified 192B exchange blob */
    std::string exInfo;
    ASSERT_EQ(seg.Export(slice, exInfo), BM_OK);
    ASSERT_EQ(exInfo.size(), UNIFIED_EXCHANGE_SEG_INFO_SIZE);

    /* VaManager classifies the extent as NVMe media */
    ASSERT_EQ(HybmVaManager::GetInstance().GetGvaMemType(slice->gva_), HYBM_MEM_TYPE_NVME);
    ASSERT_EQ(seg.UnReserveMemorySpace(), BM_OK);
}

/**
 * MemberImport_RegistersRemoteExtentAndDirection
 *  - 成员段 Import 导入属主导出信息：registry 可查、方向表吐出 NVMe 方向码。
 */
TEST_F(HybmNvmeSegmentTest, MemberImport_RegistersRemoteExtentAndDirection)
{
    auto admin = std::make_shared<MockNvmeAdmin>();
    HybmNvmeSegment owner(OwnerOptions(), 0, admin);
    void *band = nullptr;
    ASSERT_EQ(owner.ReserveMemorySpace(&band), BM_OK);
    MemSlicePtr slice;
    const uint64_t size = 2ULL * 1024 * 1024;
    ASSERT_EQ(owner.AllocLocalMemory(size, slice), BM_OK);

    std::string exInfo;
    ASSERT_EQ(owner.Export(slice, exInfo), BM_OK);

    auto memberAdmin = std::make_shared<MockNvmeAdmin>();
    MemSegmentOptions memberOpt = OwnerOptions();
    memberOpt.rankId = 1;
    HybmNvmeSegment member(memberOpt, 0, memberAdmin);
    void *memberBand = nullptr;
    ASSERT_EQ(member.ReserveMemorySpace(&memberBand), BM_OK);

    void *addresses[1] = {nullptr};
    ASSERT_EQ(member.Import({exInfo}, addresses), BM_OK);
    ASSERT_EQ(addresses[0], reinterpret_cast<void *>(slice->gva_));

    HybmNvmeExtent extent;
    ASSERT_TRUE(HybmNvmeExtentRegistry::GetInstance().Lookup(slice->gva_, size, extent));
    ASSERT_EQ(extent.ownerRank, 0U);

    /* NVMe endpoint pairs with local endpoints only, via the new direction codes */
    uint64_t localHost = 0x1000; /* plain local host pointer */
    ASSERT_EQ(HybmVaManager::GetInstance().InferCopyDirection(slice->gva_, localHost),
              HYBM_GLOBAL_NVME_TO_LOCAL_HOST);
    ASSERT_EQ(HybmVaManager::GetInstance().InferCopyDirection(localHost, slice->gva_),
              HYBM_LOCAL_HOST_TO_GLOBAL_NVME);

    /* rank-down of the owner drops the imported extent */
    ASSERT_EQ(member.RemoveImported({0U}), BM_OK);
    ASSERT_FALSE(HybmNvmeExtentRegistry::GetInstance().Lookup(slice->gva_, size, extent));
    ASSERT_EQ(owner.UnReserveMemorySpace(), BM_OK);
    ASSERT_EQ(member.UnReserveMemorySpace(), BM_OK);
}

/**
 * ReleaseSlice_CarveReuseAndUnregister
 *  - 释放回收 LBA 区间（首个空洞被第三次分配复用）；registry 注销；RemoveNs 下发。
 *    注：VA 游标只进不退（VA 廉价），复用发生在 LBA 侧。
 */
TEST_F(HybmNvmeSegmentTest, ReleaseSlice_CarveReuseAndUnregister)
{
    auto admin = std::make_shared<MockNvmeAdmin>();
    HybmNvmeSegment seg(OwnerOptions(), 0, admin);
    void *band = nullptr;
    ASSERT_EQ(seg.ReserveMemorySpace(&band), BM_OK);

    const uint64_t size = 2ULL * 1024 * 1024;
    MemSlicePtr first;
    MemSlicePtr second;
    ASSERT_EQ(seg.AllocLocalMemory(size, first), BM_OK);
    ASSERT_EQ(seg.AllocLocalMemory(size, second), BM_OK);
    ASSERT_NE(first->gva_, second->gva_);

    ASSERT_EQ(seg.ReleaseSliceMemory(first), BM_OK);
    ASSERT_EQ(admin->removed.size(), 1U);
    HybmNvmeExtent extent;
    ASSERT_FALSE(HybmNvmeExtentRegistry::GetInstance().Lookup(first->gva_, size, extent));

    /* the released LBA hole is reused: the third extent lands on the first LBA offset again
     * (VA cursor never rewinds, so its gva is fresh) */
    MemSlicePtr third;
    ASSERT_EQ(seg.AllocLocalMemory(size, third), BM_OK);
    ASSERT_NE(third->gva_, first->gva_);
    ASSERT_EQ(admin->added.size(), 3U);
    ASSERT_EQ(admin->added[2].first, admin->added[0].first);
    ASSERT_TRUE(HybmNvmeExtentRegistry::GetInstance().Lookup(third->gva_, size, extent));
    ASSERT_EQ(seg.UnReserveMemorySpace(), BM_OK);
}

/**
 * ValidateOptions_RejectsBadWindow
 *  - 非 2M 对齐窗口直接拒绝。
 */
TEST_F(HybmNvmeSegmentTest, ValidateOptions_RejectsBadWindow)
{
    auto admin = std::make_shared<MockNvmeAdmin>();
    MemSegmentOptions opt = OwnerOptions();
    opt.maxSize = 1024; /* not 2M aligned */
    HybmNvmeSegment seg(opt, 0, admin);
    ASSERT_NE(seg.ValidateOptions(), BM_OK);
}
