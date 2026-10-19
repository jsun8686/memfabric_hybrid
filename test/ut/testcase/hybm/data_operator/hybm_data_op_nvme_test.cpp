/*
 * Copyright: (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

#define private public
#define protected public
#include "dl_nvme_api.h"
#undef private
#undef protected

#include "hybm_data_op_nvme.h"

using namespace ock::mf;

namespace {
static void *const FAKE_CTRLR = reinterpret_cast<void *>(0x1000);
static void *const FAKE_NS = reinterpret_cast<void *>(0x2000);
static void *const FAKE_QPAIR = reinterpret_cast<void *>(0x3000);
constexpr uint64_t SECTOR = 512ULL;
constexpr uint64_t EXTENT_GVA = 0x40000000000ULL;

/* hand-written stub of the whole SPDK loader surface: an in-memory sector-addressed
 * "disk" plus a pending-completion queue, so the poll-to-completion loop of the data
 * operator is genuinely exercised (callbacks only fire from ProcessCompletions) */
class NvmeApiStub {
public:
    struct Cmd {
        bool write;
        uint64_t lba;
        uint32_t count;
        std::vector<char> data;
    };

    NvmeApiStub()
    {
        saved_.postInit = DlNvmeApi::gSpdkEnvDpdkPostInit;
        saved_.dmaMalloc = DlNvmeApi::gSpdkDmaMalloc;
        saved_.dmaFree = DlNvmeApi::gSpdkDmaFree;
        saved_.connect = DlNvmeApi::gSpdkNvmeConnect;
        saved_.detach = DlNvmeApi::gSpdkNvmeDetach;
        saved_.getNs = DlNvmeApi::gSpdkNvmeCtrlrGetNs;
        saved_.allocQpair = DlNvmeApi::gSpdkNvmeCtrlrAllocIoQpair;
        saved_.freeQpair = DlNvmeApi::gSpdkNvmeCtrlrFreeIoQpair;
        saved_.read = DlNvmeApi::gSpdkNvmeNsCmdRead;
        saved_.write = DlNvmeApi::gSpdkNvmeNsCmdWrite;
        saved_.process = DlNvmeApi::gSpdkNvmeQpairProcessCompletions;
        savedLoaded_ = DlNvmeApi::gLoaded;
        /* pretend the spdk libraries are already dlopen-ed: LoadLibrary turns into a
         * no-op and only the stubbed function pointers are exercised */
        DlNvmeApi::gLoaded = true;

        DlNvmeApi::gSpdkEnvDpdkPostInit = FakePostInit;
        DlNvmeApi::gSpdkDmaMalloc = FakeDmaMalloc;
        DlNvmeApi::gSpdkDmaFree = FakeDmaFree;
        DlNvmeApi::gSpdkNvmeConnect = FakeConnect;
        DlNvmeApi::gSpdkNvmeDetach = FakeDetach;
        DlNvmeApi::gSpdkNvmeCtrlrGetNs = FakeGetNs;
        DlNvmeApi::gSpdkNvmeCtrlrAllocIoQpair = FakeAllocQpair;
        DlNvmeApi::gSpdkNvmeCtrlrFreeIoQpair = FakeFreeQpair;
        DlNvmeApi::gSpdkNvmeNsCmdRead = FakeRead;
        DlNvmeApi::gSpdkNvmeNsCmdWrite = FakeWrite;
        DlNvmeApi::gSpdkNvmeQpairProcessCompletions = FakeProcessCompletions;
        current_ = this;
    }

    ~NvmeApiStub()
    {
        current_ = nullptr;
        DlNvmeApi::gSpdkEnvDpdkPostInit = saved_.postInit;
        DlNvmeApi::gSpdkDmaMalloc = saved_.dmaMalloc;
        DlNvmeApi::gSpdkDmaFree = saved_.dmaFree;
        DlNvmeApi::gSpdkNvmeConnect = saved_.connect;
        DlNvmeApi::gSpdkNvmeDetach = saved_.detach;
        DlNvmeApi::gSpdkNvmeCtrlrGetNs = saved_.getNs;
        DlNvmeApi::gSpdkNvmeCtrlrAllocIoQpair = saved_.allocQpair;
        DlNvmeApi::gSpdkNvmeCtrlrFreeIoQpair = saved_.freeQpair;
        DlNvmeApi::gSpdkNvmeNsCmdRead = saved_.read;
        DlNvmeApi::gSpdkNvmeNsCmdWrite = saved_.write;
        DlNvmeApi::gSpdkNvmeQpairProcessCompletions = saved_.process;
        DlNvmeApi::gLoaded = savedLoaded_;
    }

    void FillSector(uint64_t lba, char filler)
    {
        auto &sector = disk_[lba];
        sector.assign(SECTOR, filler);
    }

    const std::vector<char> &Sector(uint64_t lba)
    {
        return disk_[lba];
    }

    static NvmeApiStub &Instance()
    {
        return *current_;
    }

    std::vector<Cmd> cmds;
    uint32_t connectCalls = 0;
    uint32_t detachCalls = 0;
    uint32_t nsidSeen = 0;
    MfSpdkNvmeTransportId lastTrid{};

private:
    struct Saved {
        SpdkEnvDpdkPostInitFunc postInit;
        SpdkDmaMallocFunc dmaMalloc;
        SpdkDmaFreeFunc dmaFree;
        SpdkNvmeConnectFunc connect;
        SpdkNvmeDetachFunc detach;
        SpdkNvmeCtrlrGetNsFunc getNs;
        SpdkNvmeCtrlrAllocIoQpairFunc allocQpair;
        SpdkNvmeCtrlrFreeIoQpairFunc freeQpair;
        SpdkNvmeNsCmdReadFunc read;
        SpdkNvmeNsCmdWriteFunc write;
        SpdkNvmeQpairProcessCompletionsFunc process;
    };

    static int FakePostInit(bool legacyMem, void *ctx)
    {
        (void)legacyMem;
        (void)ctx;
        return 0;
    }

    static void *FakeDmaMalloc(size_t size, size_t align, uint64_t *phys)
    {
        (void)phys;
        void *buf = nullptr;
        if (posix_memalign(&buf, align < sizeof(void *) ? sizeof(void *) : align, size) != 0) {
            return nullptr;
        }
        return buf;
    }

    static void FakeDmaFree(void *buf)
    {
        free(buf);
    }

    static void *FakeConnect(MfSpdkNvmeTransportId *trid)
    {
        auto &s = Instance();
        s.connectCalls += 1;
        s.lastTrid = *trid;
        return FAKE_CTRLR;
    }

    static int FakeDetach(void *ctrlr)
    {
        (void)ctrlr;
        Instance().detachCalls += 1;
        return 0;
    }

    static void *FakeGetNs(void *ctrlr, uint32_t nsid)
    {
        (void)ctrlr;
        Instance().nsidSeen = nsid;
        return FAKE_NS;
    }

    static void *FakeAllocQpair(void *ctrlr, const void *opts, size_t optsSize)
    {
        (void)ctrlr;
        (void)opts;
        (void)optsSize;
        return FAKE_QPAIR;
    }

    static int FakeFreeQpair(void *qpair)
    {
        (void)qpair;
        return 0;
    }

    static int FakeRead(void *ns, void *qpair, void *buf, uint64_t lba, uint32_t lbaCount,
                        MfSpdkNvmeCmdCb cb, void *ctx, uint32_t ioFlags)
    {
        (void)ns;
        (void)qpair;
        (void)ioFlags;
        auto &s = Instance();
        /* data is visible once the completion fires; filling at submit is equivalent
         * here because the disk is single-threaded with the poll loop */
        for (uint32_t i = 0; i < lbaCount; ++i) {
            auto it = s.disk_.find(lba + i);
            if (it != s.disk_.end()) {
                (void)memcpy(static_cast<char *>(buf) + i * SECTOR, it->second.data(), SECTOR);
            } else {
                (void)memset(static_cast<char *>(buf) + i * SECTOR, 0x5A, SECTOR);
            }
        }
        s.cmds.push_back(Cmd{false, lba, lbaCount, {}});
        s.pending_.emplace_back(cb, ctx);
        return 0;
    }

    static int FakeWrite(void *ns, void *qpair, void *buf, uint64_t lba, uint32_t lbaCount,
                         MfSpdkNvmeCmdCb cb, void *ctx, uint32_t ioFlags)
    {
        (void)ns;
        (void)qpair;
        (void)ioFlags;
        auto &s = Instance();
        Cmd cmd{true, lba, lbaCount, {}};
        cmd.data.resize(static_cast<size_t>(lbaCount) * SECTOR);
        (void)memcpy(cmd.data.data(), buf, cmd.data.size());
        s.cmds.push_back(std::move(cmd));
        for (uint32_t i = 0; i < lbaCount; ++i) {
            auto &sector = s.disk_[lba + i];
            sector.assign(SECTOR, 0);
            (void)memcpy(sector.data(), static_cast<char *>(buf) + i * SECTOR, SECTOR);
        }
        s.pending_.emplace_back(cb, ctx);
        return 0;
    }

    static int32_t FakeProcessCompletions(void *qpair, uint32_t max)
    {
        (void)qpair;
        (void)max;
        auto &s = Instance();
        auto pending = std::move(s.pending_);
        s.pending_.clear();
        for (auto &[cb, ctx] : pending) {
            cb(ctx, nullptr);
        }
        return static_cast<int32_t>(pending.size());
    }

    Saved saved_;
    bool savedLoaded_{false};
    std::map<uint64_t, std::vector<char>> disk_;
    std::vector<std::pair<MfSpdkNvmeCmdCb, void *>> pending_;
    static NvmeApiStub *current_;
};

NvmeApiStub *NvmeApiStub::current_ = nullptr;

HybmNvmeExtent TestExtent()
{
    HybmNvmeExtent extent{};
    extent.gvaBase = EXTENT_GVA;
    extent.size = 1ULL << 30;
    extent.ownerRank = 1;
    extent.blockId = 7;
    extent.nsid = 2;
    extent.trsvcid = 4420;
    extent.lbaBase = 1024; /* absolute LBA of extent start */
    extent.lbaLen = 1ULL << 21;
    const char *nqn = "nqn.2016-06.io.spdk:cnode1";
    (void)strncpy(extent.nqn, nqn, sizeof(extent.nqn) - 1);
    const char *traddr = "10.90.22.35";
    (void)strncpy(extent.traddr, traddr, sizeof(extent.traddr) - 1);
    return extent;
}
} // namespace

class HybmDataOpNvmeTest : public testing::Test {
protected:
    void SetUp() override
    {
        HybmNvmeExtentRegistry::GetInstance().Clear();
        extent_ = TestExtent();
        HybmNvmeExtentRegistry::GetInstance().Register(extent_);
    }

    void TearDown() override
    {
        HybmNvmeExtentRegistry::GetInstance().Clear();
    }

    HybmNvmeExtent extent_;
};

/**
 * RejectsBadInput_NoLoaderTouched
 *  - 非法方向 / 未知 gva / 空指针在触达 SPDK 加载器之前被拒绝。
 */
TEST_F(HybmDataOpNvmeTest, RejectsBadInput_NoLoaderTouched)
{
    HostDataOpNvme op(0);
    ASSERT_EQ(op.Initialize(), BM_OK);

    char buf[64] = {0};
    hybm_copy_params params{};
    params.src = buf;
    params.dest = reinterpret_cast<void *>(EXTENT_GVA);
    params.dataSize = sizeof(buf);

    ASSERT_EQ(op.DataCopy(params, HYBM_LOCAL_HOST_TO_GLOBAL_HOST, ExtOptions{}), BM_INVALID_PARAM);

    hybm_copy_params nullParams = params;
    nullParams.src = nullptr;
    ASSERT_EQ(op.DataCopy(nullParams, HYBM_LOCAL_HOST_TO_GLOBAL_NVME, ExtOptions{}), BM_INVALID_PARAM);

    params.dest = reinterpret_cast<void *>(0x99900000000ULL); /* not registered */
    ASSERT_EQ(op.DataCopy(params, HYBM_LOCAL_HOST_TO_GLOBAL_NVME, ExtOptions{}), BM_INVALID_PARAM);
}

/**
 * AlignedWrite_IssuesSingleLbaCommand
 *  - 对齐写：一条 write 命令，LBA = extent.lbaBase + 偏移扇区数，数据逐字节一致，
 *    trid 按 rdma/nqn/traddr/trsvcid 填充。
 */
TEST_F(HybmDataOpNvmeTest, AlignedWrite_IssuesSingleLbaCommand)
{
    NvmeApiStub stub;
    HostDataOpNvme op(0);
    ASSERT_EQ(op.Initialize(), BM_OK);

    char src[4096];
    for (size_t i = 0; i < sizeof(src); ++i) {
        src[i] = static_cast<char>('A' + i % 26);
    }
    hybm_copy_params params{};
    params.src = src;
    params.dest = reinterpret_cast<void *>(EXTENT_GVA + 8192); /* sector 16 inside extent */
    params.dataSize = sizeof(src);
    ASSERT_EQ(op.DataCopy(params, HYBM_LOCAL_HOST_TO_GLOBAL_NVME, ExtOptions{}), BM_OK);

    ASSERT_EQ(stub.connectCalls, 1U);
    ASSERT_EQ(stub.nsidSeen, 2U);
    ASSERT_STREQ(stub.lastTrid.trstring, "rdma");
    ASSERT_STREQ(stub.lastTrid.traddr, "10.90.22.35");
    ASSERT_STREQ(stub.lastTrid.trsvcid, "4420");
    ASSERT_STREQ(stub.lastTrid.subnqn, "nqn.2016-06.io.spdk:cnode1");

    ASSERT_EQ(stub.cmds.size(), 1U);
    EXPECT_TRUE(stub.cmds[0].write);
    EXPECT_EQ(stub.cmds[0].lba, 1024U + 16U);
    EXPECT_EQ(stub.cmds[0].count, 8U);
    ASSERT_EQ(stub.cmds[0].data.size(), sizeof(src));
    EXPECT_EQ(0, memcmp(stub.cmds[0].data.data(), src, sizeof(src)));
}

/**
 * AlignedRead_PollsCompletionsAndReturnsDiskBytes
 *  - 对齐读：数据仅在 ProcessCompletions 触发回调后对调用方可见，读回内容与盘一致。
 */
TEST_F(HybmDataOpNvmeTest, AlignedRead_PollsCompletionsAndReturnsDiskBytes)
{
    NvmeApiStub stub;
    /* extent 内偏移 524288B -> LBA 2048 起 8 个扇区 */
    for (uint64_t lba = 2048; lba < 2048 + 8; ++lba) {
        stub.FillSector(lba, static_cast<char>('a' + lba % 26));
    }
    HostDataOpNvme op(0);
    ASSERT_EQ(op.Initialize(), BM_OK);

    char dst[4096] = {0};
    hybm_copy_params params{};
    params.src = reinterpret_cast<void *>(EXTENT_GVA + 524288);
    params.dest = dst;
    params.dataSize = sizeof(dst);
    ASSERT_EQ(op.DataCopy(params, HYBM_GLOBAL_NVME_TO_LOCAL_HOST, ExtOptions{}), BM_OK);

    ASSERT_EQ(stub.cmds.size(), 1U);
    EXPECT_FALSE(stub.cmds[0].write);
    EXPECT_EQ(stub.cmds[0].lba, 2048U);
    EXPECT_EQ(stub.cmds[0].count, 8U);
    for (uint64_t i = 0; i < 8; ++i) {
        EXPECT_EQ(0, memcmp(dst + i * 512, stub.Sector(2048 + i).data(), 512));
    }
}

/**
 * SubSectorWrite_DoesReadModifyWrite
 *  - 扇区内写：先读整扇区再写回合并结果，盘上其余字节保持原样。
 */
TEST_F(HybmDataOpNvmeTest, SubSectorWrite_DoesReadModifyWrite)
{
    NvmeApiStub stub;
    stub.FillSector(1024, 'X'); /* extent 第 0 扇区 */
    HostDataOpNvme op(0);
    ASSERT_EQ(op.Initialize(), BM_OK);

    const unsigned char payload[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    hybm_copy_params params{};
    params.src = const_cast<unsigned char *>(payload);
    params.dest = reinterpret_cast<void *>(EXTENT_GVA + 100); /* offset 100, len 10 */
    params.dataSize = sizeof(payload);
    ASSERT_EQ(op.DataCopy(params, HYBM_LOCAL_HOST_TO_GLOBAL_NVME, ExtOptions{}), BM_OK);

    ASSERT_EQ(stub.cmds.size(), 2U);
    EXPECT_FALSE(stub.cmds[0].write); /* RMW: read full sector first */
    EXPECT_EQ(stub.cmds[0].lba, 1024U);
    EXPECT_EQ(stub.cmds[0].count, 1U);
    EXPECT_TRUE(stub.cmds[1].write);
    EXPECT_EQ(stub.cmds[1].lba, 1024U);
    EXPECT_EQ(stub.cmds[1].count, 1U);

    const auto &sector = stub.Sector(1024);
    EXPECT_EQ(0, memcmp(sector.data(), stub.cmds[1].data.data(), 512));
    EXPECT_EQ(0, memcmp(sector.data() + 100, payload, 10));
    EXPECT_EQ(sector[0], 'X'); /* untouched head of the sector survives */
}

/**
 * SpanningWrite_SplitsHeadMidTail
 *  - 跨扇区非对齐区间：头/尾各一次 RMW，中间整扇区一条 write，盘上合并结果正确。
 */
TEST_F(HybmDataOpNvmeTest, SpanningWrite_SplitsHeadMidTail)
{
    NvmeApiStub stub;
    stub.FillSector(1024, 'H'); /* head sector */
    stub.FillSector(1026, 'T'); /* tail sector */
    HostDataOpNvme op(0);
    ASSERT_EQ(op.Initialize(), BM_OK);

    const uint64_t offset = 100;
    const uint64_t len = 1000; /* [100, 1100): head 412B, mid 512B, tail 76B */
    std::vector<char> payload(len, 'P');
    hybm_copy_params params{};
    params.src = payload.data();
    params.dest = reinterpret_cast<void *>(EXTENT_GVA + offset);
    params.dataSize = len;
    ASSERT_EQ(op.DataCopy(params, HYBM_LOCAL_HOST_TO_GLOBAL_NVME, ExtOptions{}), BM_OK);

    /* head RMW + mid write + tail RMW = 5 commands */
    ASSERT_EQ(stub.cmds.size(), 5U);
    EXPECT_FALSE(stub.cmds[0].write);
    EXPECT_EQ(stub.cmds[0].lba, 1024U);
    EXPECT_TRUE(stub.cmds[1].write);
    EXPECT_EQ(stub.cmds[1].lba, 1024U);
    EXPECT_TRUE(stub.cmds[2].write); /* middle full sector: LBA 1025 */
    EXPECT_EQ(stub.cmds[2].lba, 1025U);
    EXPECT_EQ(stub.cmds[2].count, 1U);
    EXPECT_FALSE(stub.cmds[3].write);
    EXPECT_EQ(stub.cmds[3].lba, 1026U);
    EXPECT_TRUE(stub.cmds[4].write);
    EXPECT_EQ(stub.cmds[4].lba, 1026U);

    const auto &head = stub.Sector(1024);
    EXPECT_EQ(0, memcmp(head.data() + 100, payload.data(), 412));
    EXPECT_EQ(head[0], 'H');
    EXPECT_EQ(0, memcmp(stub.Sector(1025).data(), payload.data() + 412, 512));
    const auto &tail = stub.Sector(1026);
    EXPECT_EQ(0, memcmp(tail.data(), payload.data() + 924, 76));
    EXPECT_EQ(tail[76], 'T');
}

/**
 * AsyncBatchAndWait_CompleteAndDetach
 *  - DataCopyAsync 语义等同同步；BatchDataCopy 逐项落到盘；Wait 无害；UnInitialize
 *    释放 qpair 并 detach 控制器。
 */
TEST_F(HybmDataOpNvmeTest, AsyncBatchAndWait_CompleteAndDetach)
{
    NvmeApiStub stub;
    HostDataOpNvme op(0);
    ASSERT_EQ(op.Initialize(), BM_OK);

    char src[1024];
    for (size_t i = 0; i < sizeof(src); ++i) {
        src[i] = static_cast<char>(i & 0x7F);
    }
    hybm_copy_params params{};
    params.src = src;
    params.dest = reinterpret_cast<void *>(EXTENT_GVA + 512);
    params.dataSize = sizeof(src);
    ASSERT_EQ(op.DataCopyAsync(params, HYBM_LOCAL_HOST_TO_GLOBAL_NVME, ExtOptions{}), 0);
    ASSERT_EQ(stub.cmds.size(), 1U);
    EXPECT_EQ(stub.cmds[0].lba, 1025U);

    char srcA[512];
    char srcB[512];
    (void)memset(srcA, 0x11, sizeof(srcA));
    (void)memset(srcB, 0x22, sizeof(srcB));
    void *sources[2] = {srcA, srcB};
    void *destinations[2] {reinterpret_cast<void *>(EXTENT_GVA + 1024),
                           reinterpret_cast<void *>(EXTENT_GVA + 2048)};
    const uint64_t sizes[2] = {512, 512};
    hybm_batch_copy_params batch{};
    batch.sources = sources;
    batch.destinations = destinations;
    batch.dataSizes = sizes;
    batch.batchSize = 2;
    ASSERT_EQ(op.BatchDataCopy(batch, HYBM_LOCAL_HOST_TO_GLOBAL_NVME, ExtOptions{}), BM_OK);
    EXPECT_EQ(stub.cmds.size(), 3U);
    EXPECT_EQ(0, memcmp(stub.Sector(1026).data(), srcA, 512));
    EXPECT_EQ(0, memcmp(stub.Sector(1028).data(), srcB, 512));

    ASSERT_EQ(op.Wait(0), BM_OK);

    op.UnInitialize();
    EXPECT_GE(stub.detachCalls, 1U);
}
