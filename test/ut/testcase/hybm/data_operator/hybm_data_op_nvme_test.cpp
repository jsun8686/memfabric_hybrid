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

#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

#define private public
#define protected public
#include "dl_nvme_api.h"
#include "hybm_data_op_nvme.h"
#undef private
#undef protected

using namespace ock::mf;

namespace {
static void *const FAKE_CTRLR = reinterpret_cast<void *>(0x1000);
static void *const FAKE_NS = reinterpret_cast<void *>(0x2000);
constexpr uint64_t SECTOR = 512ULL;
constexpr uint64_t EXTENT_GVA = 0x40000000000ULL;

/* hand-written stub of the whole SPDK loader surface: an in-memory sector-addressed
 * "disk" plus a pending-completion queue, so the poll-to-completion window engine of
 * the data operator is genuinely exercised (callbacks only fire from
 * ProcessCompletions). Failure injection: connectFail / submitFailAttempt /
 * processFail; observability: per-command qpair + peak in-flight depth. */
class NvmeApiStub {
public:
    struct Cmd {
        bool write;
        uint64_t lba;
        uint32_t count;
        void *qpair = nullptr;
        std::vector<char> data;
    };

    NvmeApiStub()
    {
        saved_.postInit = DlNvmeApi::gSpdkEnvDpdkPostInit;
        saved_.dmaMalloc = DlNvmeApi::gSpdkDmaMalloc;
        saved_.dmaFree = DlNvmeApi::gSpdkDmaFree;
        saved_.connect = DlNvmeApi::gSpdkNvmeConnect;
        saved_.getDefaultOpts = DlNvmeApi::gSpdkNvmeGetDefaultCtrlrOpts;
        saved_.detach = DlNvmeApi::gSpdkNvmeDetach;
        saved_.getNs = DlNvmeApi::gSpdkNvmeCtrlrGetNs;
        saved_.allocQpair = DlNvmeApi::gSpdkNvmeCtrlrAllocIoQpair;
        saved_.freeQpair = DlNvmeApi::gSpdkNvmeCtrlrFreeIoQpair;
        saved_.maxXfer = DlNvmeApi::gSpdkNvmeCtrlrGetMaxXferSize;
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
        DlNvmeApi::gSpdkNvmeGetDefaultCtrlrOpts = FakeGetDefaultCtrlrOpts; /* main 856-byte path */
        DlNvmeApi::gSpdkNvmeDetach = FakeDetach;
        DlNvmeApi::gSpdkNvmeCtrlrGetNs = FakeGetNs;
        DlNvmeApi::gSpdkNvmeCtrlrAllocIoQpair = FakeAllocQpair;
        DlNvmeApi::gSpdkNvmeCtrlrFreeIoQpair = FakeFreeQpair;
        DlNvmeApi::gSpdkNvmeCtrlrGetMaxXferSize = FakeGetMaxXferSize;
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
        DlNvmeApi::gSpdkNvmeGetDefaultCtrlrOpts = saved_.getDefaultOpts;
        DlNvmeApi::gSpdkNvmeDetach = saved_.detach;
        DlNvmeApi::gSpdkNvmeCtrlrGetNs = saved_.getNs;
        DlNvmeApi::gSpdkNvmeCtrlrAllocIoQpair = saved_.allocQpair;
        DlNvmeApi::gSpdkNvmeCtrlrFreeIoQpair = saved_.freeQpair;
        DlNvmeApi::gSpdkNvmeCtrlrGetMaxXferSize = saved_.maxXfer;
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

    /* recorded commands (submissions), in order */
    std::vector<Cmd> cmds;
    /* failure injection */
    bool connectFail = false;
    bool processFail = false;
    int64_t submitFailAttempt = -1; /* 0-based attempt index that returns -1 */
    /* observability */
    uint32_t connectCalls = 0;
    uint32_t detachCalls = 0;
    uint32_t nsidSeen = 0;
    uint32_t maxXfer = 128U * 1024U; /* reported MDTS, per-test adjustable */
    uint32_t qpairSeq = 0;
    size_t peakPending = 0;   /* max simultaneously in-flight commands */
    size_t submitAttempts = 0;
    MfSpdkNvmeTransportId lastTrid{};
    size_t lastOptsSize = 0;
    uint32_t lastKeepAliveMs = 0xFFFFFFFF;

private:
    struct Saved {
        SpdkEnvDpdkPostInitFunc postInit;
        SpdkDmaMallocFunc dmaMalloc;
        SpdkDmaFreeFunc dmaFree;
        SpdkNvmeConnectFunc connect;
        SpdkNvmeGetDefaultCtrlrOptsFunc getDefaultOpts;
        SpdkNvmeDetachFunc detach;
        SpdkNvmeCtrlrGetNsFunc getNs;
        SpdkNvmeCtrlrAllocIoQpairFunc allocQpair;
        SpdkNvmeCtrlrFreeIoQpairFunc freeQpair;
        SpdkNvmeCtrlrGetMaxXferSizeFunc maxXfer;
        SpdkNvmeNsCmdReadFunc read;
        SpdkNvmeNsCmdWriteFunc write;
        SpdkNvmeQpairProcessCompletionsFunc process;
    };

    static int FakePostInit(bool legacyMem)
    {
        (void)legacyMem;
        return 0;
    }

    /* fills a pseudo-default opts block; the sentinel KAT proves the operator
     * really overrides it to 0 (disabled) on its connect path */
    static void FakeGetDefaultCtrlrOpts(void *opts, size_t optsSize)
    {
        memset(opts, 0, optsSize);
        static_cast<MfSpdkNvmeCtrlrOptsPrefix *>(opts)->keepAliveTimeoutMs = 12345;
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

    static void *FakeConnect(const MfSpdkNvmeTransportId *trid, const void *opts, size_t optsSize)
    {
        auto &s = Instance();
        s.connectCalls += 1;
        s.lastTrid = *trid;
        s.lastOptsSize = optsSize;
        s.lastKeepAliveMs = 0xFFFFFFFF;
        if (opts != nullptr && optsSize >= sizeof(MfSpdkNvmeCtrlrOptsPrefix)) {
            s.lastKeepAliveMs = static_cast<const MfSpdkNvmeCtrlrOptsPrefix *>(opts)->keepAliveTimeoutMs;
        }
        if (s.connectFail) {
            return nullptr;
        }
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

    /* every qpair is unique: tests can attribute commands to slots */
    static void *FakeAllocQpair(void *ctrlr, const void *opts, size_t optsSize)
    {
        (void)ctrlr;
        (void)opts;
        (void)optsSize;
        auto &s = Instance();
        return reinterpret_cast<void *>(0x3000ULL + (s.qpairSeq++) * 0x100ULL);
    }

    static int FakeFreeQpair(void *qpair)
    {
        (void)qpair;
        return 0;
    }

    static uint32_t FakeGetMaxXferSize(void *ctrlr)
    {
        (void)ctrlr;
        return Instance().maxXfer;
    }

    static bool ShouldFailSubmit()
    {
        auto &s = Instance();
        const size_t attempt = s.submitAttempts;
        s.submitAttempts += 1;
        return s.submitFailAttempt >= 0 && static_cast<int64_t>(attempt) == s.submitFailAttempt;
    }

    static void TrackPending()
    {
        auto &s = Instance();
        if (s.pending_.size() > s.peakPending) {
            s.peakPending = s.pending_.size();
        }
    }

    static int FakeRead(void *ns, void *qpair, void *buf, uint64_t lba, uint32_t lbaCount,
                        MfSpdkNvmeCmdCb cb, void *ctx, uint32_t ioFlags)
    {
        (void)ns;
        (void)ioFlags;
        auto &s = Instance();
        if (ShouldFailSubmit()) {
            return -1;
        }
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
        s.cmds.push_back(Cmd{false, lba, lbaCount, qpair, {}});
        s.pending_.emplace_back(cb, ctx);
        TrackPending();
        return 0;
    }

    static int FakeWrite(void *ns, void *qpair, void *buf, uint64_t lba, uint32_t lbaCount,
                         MfSpdkNvmeCmdCb cb, void *ctx, uint32_t ioFlags)
    {
        (void)ns;
        (void)ioFlags;
        auto &s = Instance();
        if (ShouldFailSubmit()) {
            return -1;
        }
        Cmd cmd{true, lba, lbaCount, qpair, {}};
        cmd.data.resize(static_cast<size_t>(lbaCount) * SECTOR);
        (void)memcpy(cmd.data.data(), buf, cmd.data.size());
        s.cmds.push_back(std::move(cmd));
        for (uint32_t i = 0; i < lbaCount; ++i) {
            auto &sector = s.disk_[lba + i];
            sector.assign(SECTOR, 0);
            (void)memcpy(sector.data(), static_cast<char *>(buf) + i * SECTOR, SECTOR);
        }
        s.pending_.emplace_back(cb, ctx);
        TrackPending();
        return 0;
    }

    static int32_t FakeProcessCompletions(void *qpair, uint32_t max)
    {
        (void)qpair;
        (void)max;
        auto &s = Instance();
        if (s.processFail) {
            return -1;
        }
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
    }

    void TearDown() override
    {
        HybmNvmeExtentRegistry::GetInstance().SetWarmupCallback(nullptr);
        HybmNvmeExtentRegistry::GetInstance().Clear();
    }

    /* real-world order: Initialize() installs the warmup hook, THEN the extent import
     * fires it (connect + qpair pre-creation, hard-fail propagated) */
    void RegisterExtent()
    {
        ASSERT_EQ(HybmNvmeExtentRegistry::GetInstance().Register(extent_), BM_OK);
    }

    HybmNvmeExtent extent_;
};

/**
 * RejectsBadInput_NoLoaderTouched
 *  - 非法方向 / 未知 gva / 空指针在触达 SPDK 加载器之前被拒绝。
 */
TEST_F(HybmDataOpNvmeTest, RejectsBadInput_NoLoaderTouched)
{
    NvmeApiStub stub; /* spdk "loaded": Initialize preloads (A8a) before any IO */
    HostDataOpNvme op(0, "rdma");
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
 *    trid 按 rdma/nqn/traddr/trsvcid 填充；connect 发生在 extent 注册（warmup）时。
 */
TEST_F(HybmDataOpNvmeTest, AlignedWrite_IssuesSingleLbaCommand)
{
    NvmeApiStub stub;
    HostDataOpNvme op(0, "rdma");
    ASSERT_EQ(op.Initialize(), BM_OK);
    RegisterExtent(); /* warmup fires here (import time), not on the first IO */

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
    ASSERT_EQ(stub.lastOptsSize, MF_NVME_CTRLR_OPTS_SIZE); /* full upstream size, not a prefix */
    ASSERT_EQ(stub.lastKeepAliveMs, 0U); /* KAT disabled: the host never polls the admin qpair */
    ASSERT_EQ(stub.nsidSeen, 2U);
    ASSERT_STREQ(stub.lastTrid.trstring, "rdma");
    ASSERT_EQ(stub.lastTrid.trtype, 1U); /* RDMA: nvme-oF spec trtype, must be set explicitly */
    ASSERT_EQ(stub.lastTrid.adrfam, 1U); /* IPv4, an INVALID(0) adrfam is rejected by spdk clients */
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
 * LargeAlignedWrite_SplitsByMaxXfer
 *  - 超过 MDTS(128K) 的对齐写被拆成多条 <=maxXfer 的整扇区命令，LBA 连续、数据无缝拼接。
 */
TEST_F(HybmDataOpNvmeTest, LargeAlignedWrite_SplitsByMaxXfer)
{
    NvmeApiStub stub;
    stub.maxXfer = 128U * 1024U;
    HostDataOpNvme op(0, "rdma");
    ASSERT_EQ(op.Initialize(), BM_OK);
    RegisterExtent();

    const uint64_t len = 512U * 1024U; /* 512KB aligned write -> 4 x 128KB commands */
    std::vector<char> payload(len);
    for (uint64_t i = 0; i < len; ++i) {
        payload[i] = static_cast<char>('A' + (i / 4096U) % 26U);
    }
    hybm_copy_params params{};
    params.src = payload.data();
    params.dest = reinterpret_cast<void *>(EXTENT_GVA); /* sector-aligned, offset 0 */
    params.dataSize = len;
    ASSERT_EQ(op.DataCopy(params, HYBM_LOCAL_HOST_TO_GLOBAL_NVME, ExtOptions{}), BM_OK);

    ASSERT_EQ(stub.cmds.size(), 4U);
    for (uint32_t c = 0; c < 4U; ++c) {
        EXPECT_TRUE(stub.cmds[c].write);
        EXPECT_EQ(stub.cmds[c].lba, 1024U + c * 256U); /* 128KB == 256 sectors per command */
        EXPECT_EQ(stub.cmds[c].count, 256U);
        ASSERT_EQ(stub.cmds[c].data.size(), 128U * 1024U);
        EXPECT_EQ(0, memcmp(stub.cmds[c].data.data(), payload.data() + static_cast<size_t>(c) * 128U * 1024U,
                            128U * 1024U));
    }
}

/**
 * TcpTransport_BuildsTcpTrid
 *  - 池 op 位声明的协议经构造参数生效：trid.trstring == "tcp"。
 */
TEST_F(HybmDataOpNvmeTest, TcpTransport_BuildsTcpTrid)
{
    NvmeApiStub stub;
    HostDataOpNvme op(0, "tcp");
    ASSERT_EQ(op.Initialize(), BM_OK);
    RegisterExtent();

    char src[512] = {0};
    hybm_copy_params params{};
    params.src = src;
    params.dest = reinterpret_cast<void *>(EXTENT_GVA); /* one full aligned sector */
    params.dataSize = sizeof(src);
    ASSERT_EQ(op.DataCopy(params, HYBM_LOCAL_HOST_TO_GLOBAL_NVME, ExtOptions{}), BM_OK);

    ASSERT_STREQ(stub.lastTrid.trstring, "tcp");
    ASSERT_EQ(stub.lastTrid.trtype, 3U); /* TCP trtype, mapped from the transport string */
    ASSERT_EQ(stub.cmds.size(), 1U); /* full-sector direct path, no RMW */
    EXPECT_TRUE(stub.cmds[0].write);
    EXPECT_EQ(stub.cmds[0].lba, 1024U);
    EXPECT_EQ(stub.cmds[0].count, 1U);
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
    HostDataOpNvme op(0, "rdma");
    ASSERT_EQ(op.Initialize(), BM_OK);
    RegisterExtent();

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
 *  - 扇区内写：round1 读整扇区 -> merge -> round2 写回，盘上其余字节保持原样。
 */
TEST_F(HybmDataOpNvmeTest, SubSectorWrite_DoesReadModifyWrite)
{
    NvmeApiStub stub;
    stub.FillSector(1024, 'X'); /* extent 第 0 扇区 */
    HostDataOpNvme op(0, "rdma");
    ASSERT_EQ(op.Initialize(), BM_OK);
    RegisterExtent();

    const unsigned char payload[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    hybm_copy_params params{};
    params.src = const_cast<unsigned char *>(payload);
    params.dest = reinterpret_cast<void *>(EXTENT_GVA + 100); /* offset 100, len 10 */
    params.dataSize = sizeof(payload);
    ASSERT_EQ(op.DataCopy(params, HYBM_LOCAL_HOST_TO_GLOBAL_NVME, ExtOptions{}), BM_OK);

    ASSERT_EQ(stub.cmds.size(), 2U);
    EXPECT_FALSE(stub.cmds[0].write); /* RMW round 1: read full sector */
    EXPECT_EQ(stub.cmds[0].lba, 1024U);
    EXPECT_EQ(stub.cmds[0].count, 1U);
    EXPECT_TRUE(stub.cmds[1].write); /* RMW round 2: merged sector write */
    EXPECT_EQ(stub.cmds[1].lba, 1024U);
    EXPECT_EQ(stub.cmds[1].count, 1U);

    const auto &sector = stub.Sector(1024);
    EXPECT_EQ(0, memcmp(sector.data(), stub.cmds[1].data.data(), 512));
    EXPECT_EQ(0, memcmp(sector.data() + 100, payload, 10));
    EXPECT_EQ(sector[0], 'X'); /* untouched head of the sector survives */
}

/**
 * SpanningWrite_SplitsHeadMidTail
 *  - 跨扇区非对齐区间（窗口引擎时序）：round1 = head 读 + mid 写 + tail 读 共享同一窗口
 *    并发提交；round2 = head 写 + tail 写。盘上合并结果正确。
 */
TEST_F(HybmDataOpNvmeTest, SpanningWrite_SplitsHeadMidTail)
{
    NvmeApiStub stub;
    stub.FillSector(1024, 'H'); /* head sector */
    stub.FillSector(1026, 'T'); /* tail sector */
    HostDataOpNvme op(0, "rdma");
    ASSERT_EQ(op.Initialize(), BM_OK);
    RegisterExtent();

    const uint64_t offset = 100;
    const uint64_t len = 1000; /* [100, 1100): head 412B, mid 512B, tail 76B */
    std::vector<char> payload(len, 'P');
    hybm_copy_params params{};
    params.src = payload.data();
    params.dest = reinterpret_cast<void *>(EXTENT_GVA + offset);
    params.dataSize = len;
    ASSERT_EQ(op.DataCopy(params, HYBM_LOCAL_HOST_TO_GLOBAL_NVME, ExtOptions{}), BM_OK);

    /* round 1 (shared window): head read, mid write, tail read */
    ASSERT_EQ(stub.cmds.size(), 5U);
    EXPECT_FALSE(stub.cmds[0].write);
    EXPECT_EQ(stub.cmds[0].lba, 1024U);
    EXPECT_TRUE(stub.cmds[1].write); /* middle full sector: LBA 1025 */
    EXPECT_EQ(stub.cmds[1].lba, 1025U);
    EXPECT_EQ(stub.cmds[1].count, 1U);
    EXPECT_FALSE(stub.cmds[2].write);
    EXPECT_EQ(stub.cmds[2].lba, 1026U);
    /* round 2: merged head/tail writes */
    EXPECT_TRUE(stub.cmds[3].write);
    EXPECT_EQ(stub.cmds[3].lba, 1024U);
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
    HostDataOpNvme op(0, "rdma");
    ASSERT_EQ(op.Initialize(), BM_OK);
    RegisterExtent();

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

/* ---------------------- warmup / lifecycle (A8a + A8b) ---------------------- */

/**
 * WarmupFailure_FailsImport
 *  - 连接失败时 Register 硬失败（回滚条目），extent 不可见，后续 IO 报 NOT_CONNECTED
 *    —— 部署错误在 import/JOIN 时暴露，而不是首 IO。
 */
TEST_F(HybmDataOpNvmeTest, WarmupFailure_FailsImport)
{
    NvmeApiStub stub;
    stub.connectFail = true;
    HostDataOpNvme op(0, "rdma");
    ASSERT_EQ(op.Initialize(), BM_OK);

    ASSERT_NE(HybmNvmeExtentRegistry::GetInstance().Register(extent_), BM_OK);
    HybmNvmeExtent out{};
    EXPECT_FALSE(HybmNvmeExtentRegistry::GetInstance().Lookup(EXTENT_GVA, 4096, out)); /* rolled back */

    char src[512] = {0};
    hybm_copy_params params{};
    params.src = src;
    params.dest = reinterpret_cast<void *>(EXTENT_GVA);
    params.dataSize = sizeof(src);
    EXPECT_EQ(op.DataCopy(params, HYBM_LOCAL_HOST_TO_GLOBAL_NVME, ExtOptions{}), BM_NOT_CONNECTED);
}

/**
 * GetEndpointMissing_Errors
 *  - extent 注册早于 op Initialize（warmup 钩子缺席）时：IO 不做惰性连接兜底，
 *    直接 BM_NOT_CONNECTED —— 破损的装配应当显式暴露。
 */
TEST_F(HybmDataOpNvmeTest, GetEndpointMissing_Errors)
{
    NvmeApiStub stub;
    ASSERT_EQ(HybmNvmeExtentRegistry::GetInstance().Register(extent_), BM_OK); /* no callback installed */
    HostDataOpNvme op(0, "rdma");
    ASSERT_EQ(op.Initialize(), BM_OK); /* too late: the import already happened */

    char src[512] = {0};
    hybm_copy_params params{};
    params.src = src;
    params.dest = reinterpret_cast<void *>(EXTENT_GVA);
    params.dataSize = sizeof(src);
    EXPECT_EQ(op.DataCopy(params, HYBM_LOCAL_HOST_TO_GLOBAL_NVME, ExtOptions{}), BM_NOT_CONNECTED);
    EXPECT_EQ(stub.connectCalls, 0U); /* no lazy connect fallback by design */
}

/**
 * WarmupIdempotent_SecondRegisterNoReconnect
 *  - Warmup 幂等：重复 Register（例如重试的 import）不再连接。
 */
TEST_F(HybmDataOpNvmeTest, WarmupIdempotent_SecondRegisterNoReconnect)
{
    NvmeApiStub stub;
    HostDataOpNvme op(0, "rdma");
    ASSERT_EQ(op.Initialize(), BM_OK);
    RegisterExtent();
    ASSERT_EQ(HybmNvmeExtentRegistry::GetInstance().Register(extent_), BM_OK); /* re-import */
    EXPECT_EQ(stub.connectCalls, 1U);
}

/* ---------------------- window engine (B2) ---------------------- */

/**
 * BatchWindow_FillsDepthAndReaps
 *  - 窗口按 qdepth 填充：16 条命令在 qdepth=4 时峰值在飞恰为 4；全部数据落盘正确。
 */
TEST_F(HybmDataOpNvmeTest, BatchWindow_FillsDepthAndReaps)
{
    NvmeApiStub stub;
    stub.maxXfer = 512U; /* one sector per command -> 16 commands */
    (void)setenv("MF_NVME_QDEPTH", "4", 1);
    HostDataOpNvme op(0, "rdma");
    (void)unsetenv("MF_NVME_QDEPTH");
    ASSERT_EQ(op.Initialize(), BM_OK);
    ASSERT_EQ(op.qdepth_, 4U);
    RegisterExtent();

    const uint64_t len = 16U * 512U;
    std::vector<char> payload(len);
    for (uint64_t i = 0; i < len; ++i) {
        payload[i] = static_cast<char>('0' + i % 10);
    }
    hybm_copy_params params{};
    params.src = payload.data();
    params.dest = reinterpret_cast<void *>(EXTENT_GVA);
    params.dataSize = len;
    ASSERT_EQ(op.DataCopy(params, HYBM_LOCAL_HOST_TO_GLOBAL_NVME, ExtOptions{}), BM_OK);

    ASSERT_EQ(stub.cmds.size(), 16U);
    EXPECT_EQ(stub.peakPending, 4U); /* window depth honored, not a serial submit-wait */
    for (uint32_t c = 0; c < 16U; ++c) {
        EXPECT_EQ(stub.cmds[c].lba, 1024U + c);
        EXPECT_EQ(0, memcmp(stub.Sector(1024U + c).data(), payload.data() + c * 512U, 512));
    }
}

/**
 * BatchWindow_SubmitErrorDrainsInflight
 *  - 第 6 次提交失败：报错返回，但停止补发前已在飞的命令被收割完成（数据在盘）。
 */
TEST_F(HybmDataOpNvmeTest, BatchWindow_SubmitErrorDrainsInflight)
{
    NvmeApiStub stub;
    stub.maxXfer = 512U;
    stub.submitFailAttempt = 6; /* 0-based: submissions 0..5 succeed, the 7th fails */
    (void)setenv("MF_NVME_QDEPTH", "16", 1);
    HostDataOpNvme op(0, "rdma");
    (void)unsetenv("MF_NVME_QDEPTH");
    ASSERT_EQ(op.Initialize(), BM_OK);
    RegisterExtent();

    const uint64_t len = 16U * 512U;
    std::vector<char> payload(len, 'E');
    hybm_copy_params params{};
    params.src = payload.data();
    params.dest = reinterpret_cast<void *>(EXTENT_GVA);
    params.dataSize = len;
    EXPECT_EQ(op.DataCopy(params, HYBM_LOCAL_HOST_TO_GLOBAL_NVME, ExtOptions{}), BM_ERROR);

    EXPECT_EQ(stub.cmds.size(), 6U); /* issuing stopped at the failure */
    for (uint32_t c = 0; c < 6U; ++c) { /* the in-flight ones were drained and committed */
        EXPECT_EQ(0, memcmp(stub.Sector(1024U + c).data(), payload.data() + c * 512U, 512));
    }
    op.UnInitialize(); /* no staging leaks: teardown runs clean */
}

/**
 * BatchWindow_PollErrorReturns
 *  - qpair 轮询失败（链路死亡）：窗口立即报错返回，不悬挂等待永不到来的完成。
 */
TEST_F(HybmDataOpNvmeTest, BatchWindow_PollErrorReturns)
{
    NvmeApiStub stub;
    stub.processFail = true;
    HostDataOpNvme op(0, "rdma");
    ASSERT_EQ(op.Initialize(), BM_OK);
    RegisterExtent();

    char src[512] = {0};
    hybm_copy_params params{};
    params.src = src;
    params.dest = reinterpret_cast<void *>(EXTENT_GVA);
    params.dataSize = sizeof(src);
    EXPECT_EQ(op.DataCopy(params, HYBM_LOCAL_HOST_TO_GLOBAL_NVME, ExtOptions{}), BM_ERROR);
    op.UnInitialize();
}

/**
 * BatchScatter_TwoItemsAggregatedInOneWindow
 *  - 批量小 IO 聚合：两个扇区内写的 round1 两条读并发同窗、round2 两条写并发同窗
 *    （串行实现会是 R,W,R,W 交错）。
 */
TEST_F(HybmDataOpNvmeTest, BatchScatter_TwoItemsAggregatedInOneWindow)
{
    NvmeApiStub stub;
    stub.FillSector(1024, 'a');
    stub.FillSector(1026, 'b');
    HostDataOpNvme op(0, "rdma");
    ASSERT_EQ(op.Initialize(), BM_OK);
    RegisterExtent();

    char srcA[10];
    char srcB[10];
    (void)memset(srcA, 0x11, sizeof(srcA));
    (void)memset(srcB, 0x22, sizeof(srcB));
    void *sources[2] = {srcA, srcB};
    void *destinations[2] = {reinterpret_cast<void *>(EXTENT_GVA + 100),
                             reinterpret_cast<void *>(EXTENT_GVA + 1300)};
    const uint64_t sizes[2] = {10, 10};
    hybm_batch_copy_params batch{};
    batch.sources = sources;
    batch.destinations = destinations;
    batch.dataSizes = sizes;
    batch.batchSize = 2;
    ASSERT_EQ(op.BatchDataCopy(batch, HYBM_LOCAL_HOST_TO_GLOBAL_NVME, ExtOptions{}), BM_OK);

    /* both RMW read-halves submitted BEFORE either merged write: aggregated window */
    ASSERT_EQ(stub.cmds.size(), 4U);
    EXPECT_FALSE(stub.cmds[0].write);
    EXPECT_EQ(stub.cmds[0].lba, 1024U);
    EXPECT_FALSE(stub.cmds[1].write);
    EXPECT_EQ(stub.cmds[1].lba, 1026U);
    EXPECT_TRUE(stub.cmds[2].write);
    EXPECT_TRUE(stub.cmds[3].write);
    EXPECT_GE(stub.peakPending, 2U); /* they really were in flight together */

    EXPECT_EQ(0, memcmp(stub.Sector(1024).data() + 100, srcA, 10));
    EXPECT_EQ(stub.Sector(1024)[0], 'a');
    EXPECT_EQ(0, memcmp(stub.Sector(1026).data() + 276, srcB, 10)); /* 1300 - 1280 */
    EXPECT_EQ(stub.Sector(1026)[0], 'b');
}

/**
 * BatchRead_ScattersBackPerItem
 *  - 批量读：三项各一扇区，全部并发读回且各自数据正确。
 */
TEST_F(HybmDataOpNvmeTest, BatchRead_ScattersBackPerItem)
{
    NvmeApiStub stub;
    stub.FillSector(1024, 'x');
    stub.FillSector(1028, 'y');
    stub.FillSector(2048, 'z');
    HostDataOpNvme op(0, "rdma");
    ASSERT_EQ(op.Initialize(), BM_OK);
    RegisterExtent();

    char dstA[512] = {0};
    char dstB[512] = {0};
    char dstC[512] = {0};
    void *destinations[3] = {dstA, dstB, dstC};
    void *sources[3] = {reinterpret_cast<void *>(EXTENT_GVA),
                        reinterpret_cast<void *>(EXTENT_GVA + 2048),
                        reinterpret_cast<void *>(EXTENT_GVA + 524288)};
    const uint64_t sizes[3] = {512, 512, 512};
    hybm_batch_copy_params batch{};
    batch.sources = sources;
    batch.destinations = destinations;
    batch.dataSizes = sizes;
    batch.batchSize = 3;
    ASSERT_EQ(op.BatchDataCopy(batch, HYBM_GLOBAL_NVME_TO_LOCAL_HOST, ExtOptions{}), BM_OK);

    ASSERT_EQ(stub.cmds.size(), 3U);
    EXPECT_FALSE(stub.cmds[0].write);
    EXPECT_FALSE(stub.cmds[1].write);
    EXPECT_FALSE(stub.cmds[2].write);
    EXPECT_EQ(stub.peakPending, 3U); /* all three reads shared the window */
    EXPECT_EQ(dstA[0], 'x');
    EXPECT_EQ(dstB[0], 'y');
    EXPECT_EQ(dstC[0], 'z');
}

/**
 * ConcurrentCopies_UseDistinctQpairs
 *  - qpair 租赁：连续 AcquireSlot 拿到不同 slot（每窗独占一个 qpair，无全局 IO 锁）。
 */
TEST_F(HybmDataOpNvmeTest, ConcurrentCopies_UseDistinctQpairs)
{
    NvmeApiStub stub;
    HostDataOpNvme op(0, "rdma");
    ASSERT_EQ(op.Initialize(), BM_OK);
    RegisterExtent();

    NvmeEndpointPtr endpoint;
    ASSERT_EQ(op.GetEndpoint(extent_, endpoint), BM_OK);
    ASSERT_EQ(endpoint->slots.size(), 4U); /* MF_NVME_QPAIRS default */

    auto leaseA = op.AcquireSlot(endpoint);
    auto leaseB = op.AcquireSlot(endpoint);
    EXPECT_NE(leaseA.slot, leaseB.slot); /* distinct qpairs while both windows run */
    EXPECT_NE(leaseA.slot->qpair, leaseB.slot->qpair);

    leaseB.guard.unlock(); /* released: the next taker rotates to it (or another free one) */
    auto leaseC = op.AcquireSlot(endpoint);
    EXPECT_NE(leaseC.slot, leaseA.slot);
}

/**
 * QDepthEnv_Clamp
 *  - MF_NVME_QDEPTH 越界钳位到 [1, 128]；MF_NVME_BUF_SLOTS 未设时默认等于 QDEPTH。
 */
TEST_F(HybmDataOpNvmeTest, QDepthEnv_Clamp)
{
    (void)setenv("MF_NVME_QDEPTH", "999", 1);
    HostDataOpNvme opHi(0, "rdma"); /* env is read at construction, no spdk needed */
    (void)unsetenv("MF_NVME_QDEPTH");
    EXPECT_EQ(opHi.qdepth_, 128U);
    EXPECT_EQ(opHi.bufSlots_, 128U); /* default follows qdepth */

    (void)setenv("MF_NVME_QDEPTH", "0", 1);
    HostDataOpNvme opLo(0, "rdma");
    (void)unsetenv("MF_NVME_QDEPTH");
    EXPECT_EQ(opLo.qdepth_, 1U);
}
