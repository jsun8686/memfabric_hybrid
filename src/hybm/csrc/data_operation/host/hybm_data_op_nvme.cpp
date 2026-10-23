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

#include "hybm_data_op_nvme.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "dl_hybrid_api.h"
#include "hybm_ptracer.h"

namespace ock {
namespace mf {

namespace {
constexpr uint64_t NVME_SECTOR_SIZE = 512ULL;

uint64_t EnvToU64(const char *name, uint64_t defVal, uint64_t minVal, uint64_t maxVal)
{
    const char *env = getenv(name);
    if (env == nullptr || env[0] == '\0') {
        return defVal;
    }
    char *end = nullptr;
    unsigned long long val = std::strtoull(env, &end, 10);
    if (end == env) {
        return defVal;
    }
    if (val < minVal) {
        return minVal;
    }
    if (val > maxVal) {
        return maxVal;
    }
    return static_cast<uint64_t>(val);
}
} // namespace

HostDataOpNvme::HostDataOpNvme(uint32_t rankId, const std::string &trstring) noexcept : rankId_(rankId)
{
    chunkSize_ = EnvToU64("MF_NVME_BUF_MB", 4ULL, 1ULL, 1024ULL) * 1024ULL * 1024ULL;
    qdepth_ = static_cast<uint32_t>(EnvToU64("MF_NVME_QDEPTH", 16ULL, 1ULL, 128ULL));
    qpairsPerCtrlr_ = static_cast<uint32_t>(EnvToU64("MF_NVME_QPAIRS", 4ULL, 1ULL, 16ULL));
    bufSlots_ = static_cast<uint32_t>(EnvToU64("MF_NVME_BUF_SLOTS", qdepth_, 1ULL, 128ULL));
    trstring_ = trstring; /* sole source of truth: the pool op bit, no env override */
}

HostDataOpNvme::~HostDataOpNvme()
{
    UnInitialize();
}

Result HostDataOpNvme::Initialize() noexcept
{
    if (inited_) {
        return BM_OK;
    }
    /* pool create time: bring the whole spdk stack up NOW and fail the create when the
     * env is broken (a missing spdk install is a deployment error, the earlier reported
     * the better -- historically this only surfaced on the first IO) */
    TP_TRACE_BEGIN(TP_HYBM_NVME_PRELOAD);
    auto ret = EnsureReady();
    TP_TRACE_END(TP_HYBM_NVME_PRELOAD, ret);
    if (ret != BM_OK) {
        BM_LOG_ERROR("nvme data operator spdk preload failed, ret:" << ret);
        return ret;
    }
    inited_ = true;
    /* warmup hook: the extent registry pre-connects every imported extent (hard fail on
     * error, rolled back by the registry) so the first IO starts with a live endpoint */
    HybmNvmeExtentRegistry::GetInstance().SetWarmupCallback(
        [this](const HybmNvmeExtent &extent) { return Warmup(extent); });
    BM_LOG_INFO("nvme data operator initialized, rank:" << rankId_ << ", chunk:" << chunkSize_
                                                        << ", qdepth:" << qdepth_
                                                        << ", bufSlots:" << bufSlots_
                                                        << ", qpairs:" << qpairsPerCtrlr_);
    return BM_OK;
}

void HostDataOpNvme::UnInitialize() noexcept
{
    /* drop the warmup hook first: it captures `this` and would dangle after teardown */
    HybmNvmeExtentRegistry::GetInstance().SetWarmupCallback(nullptr);
    inited_ = false;
    ready_ = false;

    {
        std::lock_guard<std::mutex> guard(bufMutex_);
        for (auto *buf : freeSegBufs_) {
            DlNvmeApi::DmaFree(buf);
        }
        freeSegBufs_.clear();
        for (auto *buf : freeSectorBufs_) {
            DlNvmeApi::DmaFree(buf);
        }
        freeSectorBufs_.clear();
    }
    {
        std::lock_guard<std::mutex> guard(endpointsMutex_);
        for (auto &entry : endpoints_) {
            for (auto &slot : entry.second->slots) {
                DlNvmeApi::FreeIoQpair(slot->qpair);
            }
            DlNvmeApi::Detach(entry.second->ctrlr);
        }
        endpoints_.clear();
    }
}

void HostDataOpNvme::CleanUp() noexcept
{
    UnInitialize();
}

bool HostDataOpNvme::IsSupportedDirection(hybm_data_copy_direction direction) const noexcept
{
    switch (direction) {
        case HYBM_LOCAL_HOST_TO_GLOBAL_NVME:
        case HYBM_GLOBAL_NVME_TO_LOCAL_HOST:
        case HYBM_LOCAL_DEVICE_TO_GLOBAL_NVME:
        case HYBM_GLOBAL_NVME_TO_LOCAL_DEVICE:
            return true;
        default:
            return false;
    }
}

void HostDataOpNvme::IoDone(void *ctx, const void *cpl) noexcept
{
    if (ctx == nullptr) {
        return;
    }
    auto *io = static_cast<IoCtx *>(ctx);
    /* self-declared prefix of v25.05 struct spdk_nvme_cpl: the first 16 bits are the
     * status word (bit0 phase, bits1-8 SC, bits9-11 SCT); spdk_nvme_cpl_is_success is
     * exactly (SC == 0 && SCT == 0), i.e. (raw & 0x0FFE) == 0 */
    uint32_t status = 0;
    if (cpl != nullptr) {
        uint16_t raw = 0;
        (void)memcpy(&raw, cpl, sizeof(raw));
        if ((raw & 0x0FFE) != 0) {
            status = 1;
        }
    }
    io->status = static_cast<int32_t>(status);
    io->done = 1;
}

Result HostDataOpNvme::EnsureReady() noexcept
{
    if (ready_) {
        return BM_OK;
    }

    auto ret = DlNvmeApi::LoadLibrary();
    if (ret != BM_OK) {
        BM_LOG_ERROR("nvme data operator load spdk failed, ret:" << ret);
        return ret;
    }

    /* the host process owns the EAL lifecycle: bring DPDK up ourselves (in-memory,
     * no hugepages, no pci probing) before switching spdk to dynamic memory
     * registration mode. BM_NOT_SUPPORTED = no DPDK symbols mapped (UT stubs /
     * a host that pre-inits EAL without exposing the symbols) and is tolerated;
     * a real init failure is FATAL -- a half-dead EAL silently skips the spdk
     * mem_map subsystem and every later fabric connect dies with the cryptic
     * "Unable to create memory map" (verified: nvmf tgt + daemon co-located on
     * one node, EAL pci probing clashes with the target's RDMA nics) */
    auto ealRet = DlNvmeApi::InitDpdkEnv();
    if (ealRet != BM_OK && ealRet != BM_NOT_SUPPORTED) {
        BM_LOG_ERROR("dpdk eal init failed, ret:" << ealRet);
        return ealRet;
    }
    if (DlNvmeApi::gSpdkEnvDpdkPostInit != nullptr) {
        /* the symbol exists: its failure builds no mem_map subsystem and every
         * RDMA connect after it fails -- report it right here instead of WARN-ing
         * and deferring the surprise to the first warmup */
        const int postRet = DlNvmeApi::EnvDpdkPostInit(false);
        if (postRet != 0) {
            BM_LOG_ERROR("spdk_env_dpdk_post_init failed, ret:" << postRet);
            return BM_ERROR;
        }
    }
    ready_ = true;
    return BM_OK;
}

Result HostDataOpNvme::Warmup(const HybmNvmeExtent &extent) noexcept
{
    auto ret = EnsureReady(); /* no-op on the normal path (Initialize already preloaded) */
    if (ret != BM_OK) {
        return ret;
    }

    std::string trsvcid = std::to_string(extent.trsvcid);
    std::string key = std::string(extent.nqn) + "|" + extent.traddr + "|" + trsvcid + "|" +
                      std::to_string(extent.nsid);
    {
        std::lock_guard<std::mutex> guard(endpointsMutex_);
        if (endpoints_.find(key) != endpoints_.end()) {
            return BM_OK; /* already warmed (idempotent) */
        }
    }

    auto candidate = std::make_shared<NvmeEndpoint>();
    MfSpdkNvmeTransportId trid{};
    (void)strncpy(trid.trstring, trstring_.c_str(), sizeof(trid.trstring) - 1);
    trid.trtype = (trstring_ == "tcp") ? MF_SPDK_NVME_TRTYPE_TCP : MF_SPDK_NVME_TRTYPE_RDMA;
    (void)strncpy(trid.traddr, extent.traddr, sizeof(trid.traddr) - 1);
    (void)strncpy(trid.trsvcid, trsvcid.c_str(), sizeof(trid.trsvcid) - 1);
    (void)strncpy(trid.subnqn, extent.nqn, sizeof(trid.subnqn) - 1);
    trid.adrfam = MF_SPDK_NVMF_ADRFAM_IPV4;

    ret = DlNvmeApi::Connect(trid, candidate->ctrlr);
    if (ret != BM_OK) {
        BM_LOG_ERROR("nvme warmup connect to " << extent.traddr << ":" << extent.trsvcid
                                               << " nqn: " << extent.nqn << " nsid: " << extent.nsid
                                               << " failed, ret:" << ret);
        return BM_NOT_CONNECTED;
    }
    ret = DlNvmeApi::GetNs(candidate->ctrlr, extent.nsid, candidate->ns);
    if (ret != BM_OK) {
        BM_LOG_ERROR("nvme warmup get ns " << extent.nsid << " failed, ret:" << ret);
        DlNvmeApi::Detach(candidate->ctrlr);
        return ret;
    }
    const uint32_t xfer = DlNvmeApi::MaxXferSize(candidate->ctrlr);
    if (xfer >= NVME_SECTOR_SIZE) {
        candidate->maxXferBytes = (xfer / NVME_SECTOR_SIZE) * NVME_SECTOR_SIZE;
    }
    for (uint32_t i = 0; i < qpairsPerCtrlr_; ++i) {
        auto slot = std::make_unique<QpairSlot>();
        Result qret = DlNvmeApi::AllocIoQpair(candidate->ctrlr, slot->qpair);
        if (qret != BM_OK) {
            BM_LOG_ERROR("nvme warmup alloc io qpair failed, ret:" << qret);
            for (auto &s : candidate->slots) {
                DlNvmeApi::FreeIoQpair(s->qpair);
            }
            DlNvmeApi::Detach(candidate->ctrlr);
            return qret;
        }
        candidate->slots.push_back(std::move(slot));
    }

    {
        std::lock_guard<std::mutex> guard(endpointsMutex_);
        /* another thread may have won the race, keep the existing one and drop ours */
        auto it = endpoints_.find(key);
        if (it != endpoints_.end()) {
            for (auto &s : candidate->slots) {
                DlNvmeApi::FreeIoQpair(s->qpair);
            }
            DlNvmeApi::Detach(candidate->ctrlr);
            return BM_OK;
        }
        endpoints_[key] = candidate;
    }
    BM_LOG_INFO("nvme endpoint warmed up, nsid:" << extent.nsid << ", qpairs:" << candidate->slots.size()
                                                 << ", maxXfer:" << candidate->maxXferBytes);
    return BM_OK;
}

Result HostDataOpNvme::GetEndpoint(const HybmNvmeExtent &extent, NvmeEndpointPtr &endpoint) noexcept
{
    std::string trsvcid = std::to_string(extent.trsvcid);
    std::string key = std::string(extent.nqn) + "|" + extent.traddr + "|" + trsvcid + "|" +
                      std::to_string(extent.nsid);
    std::lock_guard<std::mutex> guard(endpointsMutex_);
    auto it = endpoints_.find(key);
    if (it == endpoints_.end()) {
        /* by design endpoints only appear via Warmup (extent import, hard-fail); reaching
         * here means the import-time warmup was skipped or lost -- refuse to connect
         * lazily so the broken setup surfaces instead of being papered over */
        BM_LOG_ERROR("nvme endpoint not warmed up (import-time warmup missing), key:" << key);
        return BM_NOT_CONNECTED;
    }
    endpoint = it->second;
    return BM_OK;
}

HostDataOpNvme::SlotLease HostDataOpNvme::AcquireSlot(const NvmeEndpointPtr &endpoint) noexcept
{
    TP_TRACE_BEGIN(TP_HYBM_NVME_QPAIR_WAIT);
    const uint32_t n = static_cast<uint32_t>(endpoint->slots.size());
    const uint32_t start = endpoint->nextSlot.fetch_add(1, std::memory_order_relaxed) % n;
    for (uint32_t i = 0; i < n; ++i) {
        auto &slot = endpoint->slots[(start + i) % n];
        std::unique_lock<std::mutex> guard(slot->mtx, std::try_to_lock);
        if (guard.owns_lock()) {
            TP_TRACE_END(TP_HYBM_NVME_QPAIR_WAIT, 0);
            return SlotLease{slot.get(), std::move(guard)};
        }
    }
    /* all slots busy: block on the rotation slot -- natural backpressure, the window
     * concurrency ceiling is the slot count (MF_NVME_QPAIRS) */
    auto &slot = endpoint->slots[start];
    std::unique_lock<std::mutex> guard(slot->mtx);
    TP_TRACE_END(TP_HYBM_NVME_QPAIR_WAIT, 0);
    return SlotLease{slot.get(), std::move(guard)};
}

void *HostDataOpNvme::AcquireBuf(bool sector) noexcept
{
    {
        std::lock_guard<std::mutex> guard(bufMutex_);
        auto &pool = sector ? freeSectorBufs_ : freeSegBufs_;
        if (!pool.empty()) {
            void *buf = pool.back();
            pool.pop_back();
            return buf;
        }
    }
    void *buf = nullptr;
    const size_t size = sector ? NVME_SECTOR_SIZE : chunkSize_;
    if (DlNvmeApi::DmaMalloc(size, NVME_SECTOR_SIZE, buf) != BM_OK) {
        return nullptr;
    }
    return buf;
}

void HostDataOpNvme::ReleaseBuf(void *buf, bool sector) noexcept
{
    if (buf == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(bufMutex_);
    auto &pool = sector ? freeSectorBufs_ : freeSegBufs_;
    if (pool.size() < bufSlots_) {
        pool.push_back(buf);
        return;
    }
    DlNvmeApi::DmaFree(buf);
}

Result HostDataOpNvme::DeviceToLocal(void *dest, const void *src, uint64_t size) noexcept
{
    return DlHybridApi::Memcpy(dest, size, src, size, ACL_MEMCPY_DEVICE_TO_HOST);
}

Result HostDataOpNvme::LocalToDevice(void *dest, const void *src, uint64_t size) noexcept
{
    return DlHybridApi::Memcpy(dest, size, src, size, ACL_MEMCPY_HOST_TO_DEVICE);
}

Result HostDataOpNvme::CopyUserOut(const NvmeCmd &cmd) noexcept
{
    if (cmd.localIsDevice) {
        return LocalToDevice(cmd.userPtr, cmd.staging + cmd.userOff, cmd.userLen);
    }
    (void)memcpy(cmd.userPtr, cmd.staging + cmd.userOff, cmd.userLen);
    return BM_OK;
}

Result HostDataOpNvme::CopyUserIn(const NvmeCmd &cmd) noexcept
{
    if (cmd.localIsDevice) {
        return DeviceToLocal(cmd.staging + cmd.userOff, cmd.userPtr, cmd.userLen);
    }
    (void)memcpy(cmd.staging + cmd.userOff, cmd.userPtr, cmd.userLen);
    return BM_OK;
}

void HostDataOpNvme::BuildCmds(const NvmeEndpointPtr &endpoint, const HybmNvmeExtent &extent, uint64_t offset,
                               char *local, uint64_t len, bool isWrite, bool localIsDevice,
                               std::vector<NvmeCmd> &cmds) noexcept
{
    const uint64_t absStart = offset;
    const uint64_t absEnd = offset + len;
    const uint64_t firstSector = absStart >> 9;
    const uint64_t lastSector = (absEnd - 1) >> 9;
    const uint32_t headSkip = static_cast<uint32_t>(absStart & (NVME_SECTOR_SIZE - 1));
    /* head clamps to len (a sub-sector write inside ONE sector must not stretch past the
     * user buffer) and tail only exists when the range actually leaves the first sector */
    const uint32_t headBytes =
        headSkip != 0 ? static_cast<uint32_t>(std::min<uint64_t>(NVME_SECTOR_SIZE - headSkip, len)) : 0U;
    const uint32_t tailBytes = (lastSector != firstSector) ? static_cast<uint32_t>(absEnd & (NVME_SECTOR_SIZE - 1)) : 0U;
    const uint64_t midStart = absStart + headBytes;
    const uint64_t midLen = len - headBytes - tailBytes;

    /* sub-sector head: one sector RMW (write path: read-half queued into the shared
     * window, merged and re-submitted in round 2; read path: full sector read, user
     * bytes sliced out on reap) */
    if (headBytes != 0) {
        NvmeCmd cmd{};
        cmd.lba = extent.lbaBase + firstSector;
        cmd.lbaCount = 1;
        cmd.rmw = true;
        cmd.isWrite = false;
        cmd.pendingWrite = isWrite;
        cmd.userPtr = local;
        cmd.userOff = headSkip;
        cmd.userLen = headBytes;
        cmd.localIsDevice = localIsDevice;
        cmds.push_back(cmd);
    }

    /* mid: full-sector direct IO, split by min(ctrlr MDTS, staging chunk); spdk does not
     * split oversized IO itself, a target (e.g. nvmf TCP, 128K default) would reject it */
    if (midLen != 0) {
        const uint64_t segCap = std::min<uint64_t>(std::min<uint64_t>(endpoint->maxXferBytes, chunkSize_),
                                                   midLen) &
                                ~(NVME_SECTOR_SIZE - 1);
        uint64_t submitted = 0;
        while (submitted < midLen) {
            const uint64_t n = std::min<uint64_t>(segCap, midLen - submitted);
            NvmeCmd cmd{};
            cmd.lba = extent.lbaBase + ((midStart + submitted) >> 9);
            cmd.lbaCount = static_cast<uint32_t>(n >> 9);
            cmd.isWrite = isWrite;
            cmd.userPtr = local + headBytes + submitted;
            cmd.userLen = static_cast<uint32_t>(n);
            cmd.localIsDevice = localIsDevice;
            cmds.push_back(cmd);
            submitted += n;
        }
    }

    if (tailBytes != 0) {
        NvmeCmd cmd{};
        cmd.lba = extent.lbaBase + lastSector;
        cmd.lbaCount = 1;
        cmd.rmw = true;
        cmd.isWrite = false;
        cmd.pendingWrite = isWrite;
        cmd.userPtr = local + (len - tailBytes);
        cmd.userOff = 0;
        cmd.userLen = tailBytes;
        cmd.localIsDevice = localIsDevice;
        cmds.push_back(cmd);
    }
}

Result HostDataOpNvme::PrepareAndSubmit(const NvmeEndpointPtr &endpoint, QpairSlot *slot, NvmeCmd &cmd) noexcept
{
    if (cmd.staging == nullptr) {
        /* staging is taken lazily here (NOT in BuildCmds): only qdepth commands are in
         * flight at any time, so the pools only need qdepth blocks */
        cmd.staging = static_cast<char *>(AcquireBuf(cmd.rmw));
        if (cmd.staging == nullptr) {
            BM_LOG_ERROR("nvme staging buffer exhausted");
            return BM_MALLOC_FAILED;
        }
        /* mid writes stage the user bytes in before submit; reads and RMW read-halves
         * are filled by the device */
        if (cmd.isWrite && !cmd.pendingWrite) {
            const uint64_t t0 = TP_CURRENT_TIME_NS;
            auto ret = CopyUserIn(cmd);
            TP_TRACE_RECORD(TP_HYBM_NVME_STAGE_COPY, TP_CURRENT_TIME_NS - t0, ret);
            if (ret != BM_OK) {
                return ret;
            }
        }
    }
    /* else: round-2 RMW command -- the staging (sector already read + merged by ReapCmd)
     * rides along, no re-acquire (would leak the transferred block) and no re-merge */
    const int rc = cmd.isWrite
        ? DlNvmeApi::NsCmdWrite(endpoint->ns, slot->qpair, cmd.staging, cmd.lba, cmd.lbaCount, IoDone, &cmd.ctx)
        : DlNvmeApi::NsCmdRead(endpoint->ns, slot->qpair, cmd.staging, cmd.lba, cmd.lbaCount, IoDone, &cmd.ctx);
    TP_TRACE_RECORD(TP_HYBM_NVME_SUBMIT, 0, rc == 0 ? 0 : 1);
    if (rc != 0) {
        BM_LOG_ERROR("nvme submit " << (cmd.isWrite ? "write" : "read") << " failed, rc:" << rc
                                    << ", lba:" << cmd.lba << ", lbaCount:" << cmd.lbaCount);
        return BM_ERROR;
    }
    return BM_OK;
}

Result HostDataOpNvme::ReapCmd(NvmeCmd &cmd, std::vector<NvmeCmd> *nextRound) noexcept
{
    if (cmd.pendingWrite) {
        /* write-path RMW read-half done: merge the user bytes into the sector and hand
         * the very same staging buffer to the round-2 write command */
        const uint64_t t0 = TP_CURRENT_TIME_NS;
        auto ret = CopyUserIn(cmd);
        TP_TRACE_RECORD(TP_HYBM_NVME_STAGE_COPY, TP_CURRENT_TIME_NS - t0, ret);
        if (ret != BM_OK) {
            ReleaseBuf(cmd.staging, cmd.rmw);
            cmd.released = true;
            return ret;
        }
        NvmeCmd wr = cmd;
        wr.isWrite = true;
        wr.pendingWrite = false;
        wr.finished = false;
        wr.released = false;
        wr.ctx = IoCtx{};
        cmd.released = true; /* staging ownership moved to the round-2 command */
        nextRound->push_back(wr);
        return BM_OK;
    }
    if (!cmd.isWrite) {
        /* read: slice the user bytes out of staging into the local side */
        const uint64_t t0 = TP_CURRENT_TIME_NS;
        auto ret = CopyUserOut(cmd);
        TP_TRACE_RECORD(TP_HYBM_NVME_STAGE_COPY, TP_CURRENT_TIME_NS - t0, ret);
        ReleaseBuf(cmd.staging, cmd.rmw);
        cmd.released = true;
        return ret;
    }
    /* mid write: the CQE is the acknowledgment, staging is free */
    ReleaseBuf(cmd.staging, cmd.rmw);
    cmd.released = true;
    return BM_OK;
}

Result HostDataOpNvme::SubmitWindowed(const NvmeEndpointPtr &endpoint, QpairSlot *slot,
                                      std::vector<NvmeCmd> &cmds, std::vector<NvmeCmd> *nextRound) noexcept
{
    size_t next = 0;  /* first not-yet-submitted command */
    size_t base = 0;  /* first not-yet-reaped command */
    uint32_t inflight = 0;
    Result firstErr = BM_OK;

    while (base < cmds.size() || inflight > 0) {
        /* fill the window up to qdepth */
        while (firstErr == BM_OK && inflight < qdepth_ && next < cmds.size()) {
            auto ret = PrepareAndSubmit(endpoint, slot, cmds[next]);
            if (ret != BM_OK) {
                firstErr = ret; /* stop issuing, keep draining what is in flight */
                break;
            }
            ++next;
            ++inflight;
        }
        if (inflight == 0) {
            break; /* everything submitted got reaped (or nothing could be submitted) */
        }

        TP_TRACE_BEGIN(TP_HYBM_NVME_POLL_WAIT);
        const int32_t reaped = DlNvmeApi::ProcessCompletions(slot->qpair);
        TP_TRACE_END(TP_HYBM_NVME_POLL_WAIT, reaped < 0 ? 1 : 0);
        if (reaped < 0) {
            BM_LOG_ERROR("nvme qpair process completions failed");
            firstErr = firstErr == BM_OK ? BM_ERROR : firstErr;
            break; /* the qpair is dead: completions will never arrive, abandon them */
        }

        for (size_t i = base; i < next; ++i) {
            NvmeCmd &cmd = cmds[i];
            if (cmd.finished || cmd.ctx.done == 0) {
                continue;
            }
            cmd.finished = true;
            --inflight;
            if (cmd.ctx.status != 0 && firstErr == BM_OK) {
                BM_LOG_ERROR("nvme command failed in cpl, lba:" << cmd.lba << ", lbaCount:" << cmd.lbaCount);
                firstErr = BM_ERROR;
            }
            if (firstErr == BM_OK) {
                auto ret = ReapCmd(cmd, nextRound);
                if (ret != BM_OK) {
                    firstErr = ret;
                }
            } else if (!cmd.released) {
                /* error already recorded: just drop the staging, no side effects */
                ReleaseBuf(cmd.staging, cmd.rmw);
                cmd.released = true;
            }
        }
        while (base < next && cmds[base].finished) {
            ++base;
        }
    }
    return firstErr;
}

void HostDataOpNvme::ReleaseCmds(std::vector<NvmeCmd> &cmds) noexcept
{
    for (auto &cmd : cmds) {
        if (!cmd.released && cmd.staging != nullptr) {
            ReleaseBuf(cmd.staging, cmd.rmw);
            cmd.released = true;
        }
    }
}

Result HostDataOpNvme::RunItems(std::vector<CopyItem> &items, bool isWrite, bool localIsDevice) noexcept
{
    /* resolve every item to (extent, endpoint) and group per endpoint: a window is bound
     * to one qpair slot, commands of different endpoints cannot share it */
    struct Group {
        NvmeEndpointPtr endpoint;
        HybmNvmeExtent extent;
        std::vector<size_t> itemIdx;
    };
    std::vector<Group> groups;
    std::map<std::string, size_t> groupByKey;
    for (size_t i = 0; i < items.size(); ++i) {
        TP_TRACE_BEGIN(TP_HYBM_NVME_LOOKUP);
        HybmNvmeExtent extent;
        const bool found = HybmNvmeExtentRegistry::GetInstance().Lookup(items[i].gva, items[i].size, extent);
        TP_TRACE_END(TP_HYBM_NVME_LOOKUP, found ? 0 : 1);
        if (!found) {
            BM_LOG_ERROR("gva 0x" << std::hex << items[i].gva << std::dec << " size " << items[i].size
                                  << " is not a registered nvme extent");
            return BM_INVALID_PARAM;
        }
        NvmeEndpointPtr endpoint;
        auto ret = GetEndpoint(extent, endpoint);
        if (ret != BM_OK) {
            return ret;
        }
        const std::string key = std::string(extent.nqn) + "|" + extent.traddr + "|" +
                                std::to_string(extent.trsvcid) + "|" + std::to_string(extent.nsid);
        auto it = groupByKey.find(key);
        if (it == groupByKey.end()) {
            groupByKey[key] = groups.size();
            Group group;
            group.endpoint = endpoint;
            group.extent = extent;
            groups.push_back(std::move(group));
            it = groupByKey.find(key);
        }
        groups[it->second].itemIdx.push_back(i);
    }

    /* batch semantics follow host_rdma: a failing group is recorded and the remaining
     * groups still run; the first error is returned after everyone got its chance */
    Result firstErr = BM_OK;
    for (auto &group : groups) {
        std::vector<NvmeCmd> cmds;
        cmds.reserve(group.itemIdx.size() * 2U + group.itemIdx.size());
        for (size_t idx : group.itemIdx) {
            const auto &item = items[idx];
            BuildCmds(group.endpoint, group.extent, item.gva - group.extent.gvaBase, item.local, item.size,
                      isWrite, localIsDevice, cmds);
        }

        auto lease = AcquireSlot(group.endpoint);
        std::vector<NvmeCmd> stageB;
        auto ret = SubmitWindowed(group.endpoint, lease.slot, cmds, &stageB);
        if (ret == BM_OK && !stageB.empty()) {
            TP_TRACE_BEGIN(TP_HYBM_NVME_RMW_STAGE_B);
            ret = SubmitWindowed(group.endpoint, lease.slot, stageB, nullptr);
            TP_TRACE_END(TP_HYBM_NVME_RMW_STAGE_B, ret);
        }
        ReleaseCmds(cmds);
        ReleaseCmds(stageB);
        if (ret != BM_OK) {
            BM_LOG_ERROR("nvme window failed for " << group.itemIdx.size() << " item(s), ret:" << ret);
            if (firstErr == BM_OK) {
                firstErr = ret;
            }
        }
    }
    return firstErr;
}

Result HostDataOpNvme::DataCopy(hybm_copy_params &params, hybm_data_copy_direction direction,
                                const ExtOptions &options) noexcept
{
    (void)options;
    if (!inited_ || params.src == nullptr || params.dest == nullptr || params.dataSize == 0) {
        return BM_INVALID_PARAM;
    }
    if (!IsSupportedDirection(direction)) {
        return BM_INVALID_PARAM;
    }

    const bool isWrite = direction == HYBM_LOCAL_HOST_TO_GLOBAL_NVME || direction == HYBM_LOCAL_DEVICE_TO_GLOBAL_NVME;
    const bool localIsDevice =
        direction == HYBM_LOCAL_DEVICE_TO_GLOBAL_NVME || direction == HYBM_GLOBAL_NVME_TO_LOCAL_DEVICE;

    TP_TRACE_BEGIN(TP_HYBM_NVME_COPY_TOTAL);
    CopyItem item;
    item.local = static_cast<char *>(isWrite ? params.src : params.dest);
    item.gva = reinterpret_cast<uint64_t>(isWrite ? params.dest : params.src);
    item.size = params.dataSize;
    std::vector<CopyItem> items{item};
    const Result ret = RunItems(items, isWrite, localIsDevice);
    TP_TRACE_END(TP_HYBM_NVME_COPY_TOTAL, ret);
    return ret;
}

Result HostDataOpNvme::DataCopyAsync(hybm_copy_params &params, hybm_data_copy_direction direction,
                                     const ExtOptions &options) noexcept
{
    /* executed synchronously, the wait contract is preserved (Wait is a no-op drain);
     * deep async pipelining is a follow-up optimization */
    auto ret = DataCopy(params, direction, options);
    return ret == BM_OK ? 0 : ret;
}

Result HostDataOpNvme::BatchDataCopy(hybm_batch_copy_params &params, hybm_data_copy_direction direction,
                                     const ExtOptions &options) noexcept
{
    (void)options;
    if (!inited_ || params.sources == nullptr || params.destinations == nullptr || params.dataSizes == nullptr ||
        params.batchSize == 0) {
        return BM_INVALID_PARAM;
    }
    if (!IsSupportedDirection(direction)) {
        return BM_INVALID_PARAM;
    }

    const bool isWrite = direction == HYBM_LOCAL_HOST_TO_GLOBAL_NVME || direction == HYBM_LOCAL_DEVICE_TO_GLOBAL_NVME;
    const bool localIsDevice =
        direction == HYBM_LOCAL_DEVICE_TO_GLOBAL_NVME || direction == HYBM_GLOBAL_NVME_TO_LOCAL_DEVICE;

    TP_TRACE_BEGIN(TP_HYBM_NVME_COPY_TOTAL);
    std::vector<CopyItem> items;
    items.reserve(params.batchSize);
    for (uint32_t i = 0; i < params.batchSize; ++i) {
        CopyItem item;
        item.local = static_cast<char *>(isWrite ? params.sources[i] : params.destinations[i]);
        item.gva = reinterpret_cast<uint64_t>(isWrite ? params.destinations[i] : params.sources[i]);
        item.size = params.dataSizes[i];
        items.push_back(item);
    }
    /* the WHOLE batch expands into one command sequence per endpoint: small IOs ride the
     * same window and pipeline together instead of paying a serial round-trip each */
    const Result ret = RunItems(items, isWrite, localIsDevice);
    TP_TRACE_END(TP_HYBM_NVME_COPY_TOTAL, ret);
    return ret;
}

Result HostDataOpNvme::Wait(int32_t waitId) noexcept
{
    (void)waitId;
    /* the window path polls its own qpairs to completion; drain stragglers defensively */
    std::vector<NvmeEndpointPtr> snapshot;
    {
        std::lock_guard<std::mutex> guard(endpointsMutex_);
        for (auto &entry : endpoints_) {
            snapshot.push_back(entry.second);
        }
    }
    for (auto &endpoint : snapshot) {
        for (auto &slot : endpoint->slots) {
            (void)DlNvmeApi::ProcessCompletions(slot->qpair);
        }
    }
    return BM_OK;
}

} // namespace mf
} // namespace ock
