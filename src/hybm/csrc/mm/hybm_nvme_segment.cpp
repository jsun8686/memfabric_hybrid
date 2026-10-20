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

#include "hybm_nvme_segment.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <sstream>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "hybm_logger.h"
#include "hybm_mem_slice.h"
#include "hybm_va_manager.h"

namespace ock {
namespace mf {

/* ---------------- registry ---------------- */

HybmNvmeExtentRegistry &HybmNvmeExtentRegistry::GetInstance()
{
    static HybmNvmeExtentRegistry instance;
    return instance;
}

void HybmNvmeExtentRegistry::Register(const HybmNvmeExtent &extent)
{
    std::lock_guard<std::mutex> guard(mutex_);
    extents_[extent.gvaBase] = extent;
}

void HybmNvmeExtentRegistry::UnregisterByGva(uint64_t gvaBase)
{
    std::lock_guard<std::mutex> guard(mutex_);
    extents_.erase(gvaBase);
}

void HybmNvmeExtentRegistry::UnregisterByRank(uint32_t rank)
{
    std::lock_guard<std::mutex> guard(mutex_);
    for (auto it = extents_.begin(); it != extents_.end();) {
        it = (it->second.ownerRank == rank) ? extents_.erase(it) : std::next(it);
    }
}

bool HybmNvmeExtentRegistry::Lookup(uint64_t gva, uint64_t size, HybmNvmeExtent &out) const
{
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = extents_.upper_bound(gva);
    if (it == extents_.begin()) {
        return false;
    }
    --it;
    const auto &extent = it->second;
    if (gva >= extent.gvaBase && gva + size <= extent.gvaBase + extent.size) {
        out = extent;
        return true;
    }
    return false;
}

void HybmNvmeExtentRegistry::Clear()
{
    std::lock_guard<std::mutex> guard(mutex_);
    extents_.clear();
}

/* ---------------- env helpers ---------------- */

static std::string NvmeEnvStr(const char *name, const char *dft)
{
    const char *val = getenv(name);
    return (val != nullptr && val[0] != '\0') ? std::string(val) : std::string(dft);
}

static uint64_t NvmeEnvU64(const char *name, uint64_t dft)
{
    const char *val = getenv(name);
    if (val == nullptr || val[0] == '\0') {
        return dft;
    }
    return strtoull(val, nullptr, 0);
}

std::string SpdkNvmfAdminClient::GetNqn()
{
    return NvmeEnvStr("MF_NVME_NQN", "");
}

std::string SpdkNvmfAdminClient::GetTraddr()
{
    return NvmeEnvStr("MF_NVME_TRADDR", "");
}

uint32_t SpdkNvmfAdminClient::GetTrsvcid()
{
    return static_cast<uint32_t>(NvmeEnvU64("MF_NVME_TRSVCID", 4420U));
}

/* one newline-delimited JSON-RPC roundtrip over the unix socket */
bool SpdkNvmfAdminClient::Roundtrip(const std::string &request, std::string &response)
{
    const std::string sockPath = NvmeEnvStr("MF_NVME_RPC_SOCK", "/var/tmp/spdk.sock");
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        BM_LOG_ERROR("nvme admin: create unix socket failed, errno: " << errno);
        return false;
    }
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    (void)strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        BM_LOG_ERROR("nvme admin: connect " << sockPath << " failed, errno: " << errno
                                            << " -- is the nvmf target up?");
        close(fd);
        return false;
    }
    const std::string line = request + "\n";
    size_t sent = 0;
    while (sent < line.size()) {
        auto n = send(fd, line.data() + sent, line.size() - sent, 0);
        if (n <= 0) {
            BM_LOG_ERROR("nvme admin: send rpc failed, errno: " << errno);
            close(fd);
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    std::ostringstream buf;
    char chunk[4096];
    while (true) {
        auto n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            BM_LOG_ERROR("nvme admin: recv rpc failed, errno: " << errno);
            close(fd);
            return false;
        }
        buf.write(chunk, n);
        std::string acc = buf.str();
        if (acc.find('\n') != std::string::npos) {
            break; /* single-line JSON-RPC response */
        }
    }
    close(fd);
    response = buf.str();
    return true;
}

/* minimal field scan: find "key":<number> in a flat JSON response */
static bool JsonScanNumber(const std::string &json, const char *key, int64_t &out)
{
    std::string needle = std::string("\"") + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) {
        return false;
    }
    out = strtoll(json.c_str() + pos + needle.size(), nullptr, 10);
    return true;
}

Result SpdkNvmfAdminClient::QueryTrtype(const std::string &expectedTrtype)
{
    std::string resp;
    if (!Roundtrip("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"nvmf_get_transports\"}", resp)) {
        return BM_NOT_CONNECTED;
    }
    /* expected arrives lowercase from the op bit ("tcp"/"rdma"); spdk reports uppercase */
    std::string want = expectedTrtype;
    (void)std::transform(want.begin(), want.end(), want.begin(), ::toupper);
    /* accept both the compact spdk form and a spaced python-json form */
    const std::string compact = "\"trtype\":\"" + want + "\"";
    const std::string spaced = "\"trtype\": \"" + want + "\"";
    if (resp.find(compact) != std::string::npos || resp.find(spaced) != std::string::npos) {
        return BM_OK;
    }
    BM_LOG_ERROR("nvme admin: pool declares NVMe-oF over " << expectedTrtype
                                                            << " but the target does not listen on it: "
                                                            << resp.substr(0, 200));
    return BM_NOT_SUPPORTED;
}

static bool JsonHasError(const std::string &json)
{
    return json.find("\"error\":{") != std::string::npos || json.find("\"code\":-") != std::string::npos;
}

Result SpdkNvmfAdminClient::AddNs(uint64_t lbaOffset, uint64_t lbaLen, uint32_t &nsid)
{
    /* extent -> run of same-size splits of the base bdev -> one linear bdev -> one namespace.
     * the split table is created once (uniform split granularity); each extent then owns
     * [lbaOffset/gran, (lbaOffset+lbaLen)/gran) consecutive splits */
    const uint64_t gran = NvmeEnvU64("MF_NVME_EXTENT_GRAN", 1ULL << 30);
    const std::string baseBdev = NvmeEnvStr("MF_NVME_BASE_BDEV", "Nvme0n1");
    const std::string nqn = GetNqn();
    if (nqn.empty()) {
        BM_LOG_ERROR("nvme admin: MF_NVME_NQN is not set, cannot expose extents");
        return BM_INVALID_PARAM;
    }
    const uint64_t firstSplit = lbaOffset / gran;
    const uint64_t splitCount = lbaLen / gran;
    std::ostringstream req;
    req << "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"bdev_split_create\",\"params\":{\"base_bdev\":\""
        << baseBdev << "\",\"split_size\":" << (gran / 512ULL) << ",\"split_count\":" << splitCount << "}}";
    std::string resp;
    if (!Roundtrip(req.str(), resp)) {
        return BM_NOT_CONNECTED;
    }
    if (JsonHasError(resp)) {
        /* table may already exist (same granularity): tolerate and continue */
        BM_LOG_WARN("nvme admin: bdev_split_create returned an error (may pre-exist): " << resp);
    }

    /* linear bdev over this extent's split run */
    std::ostringstream name;
    name << baseBdev << "n" << firstSplit << "x" << splitCount;
    std::ostringstream raidReq;
    raidReq << "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"bdev_raid_create\",\"params\":{\"name\":\""
            << name.str() << "\",\"raid_level\":\"linear\",\"base_bdevs\":[";
    for (uint64_t i = 0; i < splitCount; i++) {
        raidReq << (i > 0 ? "," : "") << "\"" << baseBdev << "n" << (firstSplit + i) << "\"";
    }
    raidReq << "]}}";
    if (!Roundtrip(raidReq.str(), resp)) {
        return BM_NOT_CONNECTED;
    }
    if (JsonHasError(resp)) {
        BM_LOG_ERROR("nvme admin: bdev_raid_create failed: " << resp);
        return BM_ERROR;
    }

    std::ostringstream nsReq;
    nsReq << "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"nvmf_subsystem_add_ns\",\"params\":{\"nqn\":\"" << nqn
          << "\",\"bdev_name\":\"" << name.str() << "\"}}";
    if (!Roundtrip(nsReq.str(), resp)) {
        return BM_NOT_CONNECTED;
    }
    if (JsonHasError(resp)) {
        BM_LOG_ERROR("nvme admin: nvmf_subsystem_add_ns failed: " << resp);
        return BM_ERROR;
    }
    int64_t parsedNsid = 0;
    if (!JsonScanNumber(resp, "nsid", parsedNsid) || parsedNsid <= 0) {
        BM_LOG_ERROR("nvme admin: add_ns response carries no nsid: " << resp);
        return BM_ERROR;
    }
    nsid = static_cast<uint32_t>(parsedNsid);
    BM_LOG_INFO("nvme admin: extent exposed, lbaOffset: " << lbaOffset << " lbaLen: " << lbaLen
                                                          << " bdev: " << name.str() << " nsid: " << nsid);
    return BM_OK;
}

Result SpdkNvmfAdminClient::RemoveNs(uint32_t nsid)
{
    const std::string nqn = GetNqn();
    if (nqn.empty()) {
        return BM_INVALID_PARAM;
    }
    std::ostringstream req;
    req << "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"nvmf_subsystem_remove_ns\",\"params\":{\"nqn\":\"" << nqn
        << "\",\"nsid\":" << nsid << "}}";
    std::string resp;
    if (!Roundtrip(req.str(), resp)) {
        return BM_NOT_CONNECTED;
    }
    if (JsonHasError(resp)) {
        BM_LOG_WARN("nvme admin: remove_ns nsid " << nsid << " returned an error (may be gone): " << resp);
    }
    return BM_OK; /* volatile pool: a stale ns is destroyed with the subsystem anyway */
}

bool SpdkNvmfAdminClient::Healthy()
{
    std::string resp;
    return Roundtrip("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"framework_get_rpc_methods\"}", resp);
}

/* ---------------- segment ---------------- */

HybmNvmeSegment::HybmNvmeSegment(const MemSegmentOptions &options, int eid,
                                 std::shared_ptr<HybmNvmeAdminClient> client)
    : MemSegment{options, eid}, admin_{client != nullptr ? std::move(client)
                                                         : std::make_shared<SpdkNvmfAdminClient>()}
{}

HybmNvmeSegment::~HybmNvmeSegment()
{
    UnReserveMemorySpace();
}

Result HybmNvmeSegment::ValidateOptions() noexcept
{
    if (options_.maxSize == 0 || (options_.maxSize % HYBM_LARGE_PAGE_SIZE) != 0) {
        BM_LOG_ERROR("nvme segment maxSize invalid: " << options_.maxSize);
        return BM_INVALID_PARAM;
    }
    if (options_.rankCnt == 0) {
        BM_LOG_ERROR("nvme segment rankCnt is 0");
        return BM_INVALID_PARAM;
    }
    return BM_OK;
}

Result HybmNvmeSegment::ReserveMemorySpace(void **address) noexcept
{
    auto ret = ValidateOptions();
    if (ret != BM_OK) {
        return ret;
    }
    totalVirtualSize_ = options_.rankCnt * options_.maxSize;
    /* GVA-only band: no LVA is reserved, nothing is ever mapped (see AllocReserveGva) */
    auto gvaInfo = HybmVaManager::GetInstance().AllocReserveGva(options_.rankId, totalVirtualSize_, 0,
                                                                HYBM_MEM_TYPE_NVME, options_.enable56BitsGva);
    if (gvaInfo.va[HVM_GVA] == 0) {
        BM_LOG_ERROR("nvme segment reserve gva band failed, size: " << totalVirtualSize_);
        return BM_ERROR;
    }
    globalVirtualAddress_ = reinterpret_cast<uint8_t *>(gvaInfo.va[HVM_GVA]);

    granularity_ = NvmeEnvU64("MF_NVME_EXTENT_GRAN", granularity_);
    if (granularity_ < HYBM_LARGE_PAGE_SIZE) {
        granularity_ = HYBM_LARGE_PAGE_SIZE;
    }
    lbaBaseEnv_ = NvmeEnvU64("MF_NVME_LBA_BASE", 0);
    lbaCapacity_ = (options_.maxSize / granularity_) * granularity_; /* whole splits only */
    freeLba_.clear();
    freeLba_.emplace(0, lbaCapacity_);

    if (!admin_->Healthy()) {
        /* keep the band: the target may come up later; allocations fail fast meanwhile and the
         * capacity report drops to zero so placement stops choosing this node */
        BM_LOG_WARN("nvme segment: local nvmf target not reachable now, extents cannot be exposed yet");
    } else {
        /* protocol consistency: the pool op bit (carried to this FAR entity by the
         * create/JOIN_ALLOC control plane) declares the transport the initiators speak;
         * a mismatch blocks the segment instead of failing on every first IO */
        const char *expected = nullptr;
        if ((options_.dataOpType & HYBM_DOP_TYPE_NVME_TCP) != 0) {
            expected = "tcp";
        } else if ((options_.dataOpType & HYBM_DOP_TYPE_NVME_RDMA) != 0) {
            expected = "rdma";
        }
        if (expected != nullptr) {
            auto qret = admin_->QueryTrtype(expected);
            if (qret != BM_OK) {
                BM_LOG_ERROR("nvme segment: pool op bit declares " << expected
                                                                   << " but the target does not listen on it");
                return qret;
            }
            BM_LOG_INFO("nvme segment: target transport matches the pool op bit (" << expected << ")");
        }
    }
    BM_LOG_INFO("nvme segment band reserved, gva: " << VaToStr(reinterpret_cast<uint64_t>(globalVirtualAddress_))
                                                    << " window: " << totalVirtualSize_
                                                    << " lbaCapacity: " << lbaCapacity_
                                                    << " granularity: " << granularity_);
    if (address != nullptr) {
        *address = globalVirtualAddress_;
    }
    return BM_OK;
}

Result HybmNvmeSegment::UnReserveMemorySpace() noexcept
{
    for (const auto &it : sliceExtents_) {
        (void)admin_->RemoveNs(it.second.nsid);
        HybmNvmeExtentRegistry::GetInstance().UnregisterByGva(it.second.gva);
    }
    slices_.clear();
    sliceExtents_.clear();
    exportMap_.clear();
    for (const auto &im : imports_) {
        HybmNvmeExtentRegistry::GetInstance().UnregisterByGva(im.gva);
    }
    imports_.clear();
    freeLba_.clear();
    if (globalVirtualAddress_ != nullptr) {
        HybmVaManager::GetInstance().FreeReserveGva(reinterpret_cast<uint64_t>(globalVirtualAddress_));
        globalVirtualAddress_ = nullptr;
    }
    return BM_OK;
}

bool HybmNvmeSegment::AllocLbaRange(uint64_t size, uint64_t &offsetOut)
{
    for (auto it = freeLba_.begin(); it != freeLba_.end(); ++it) {
        if (it->second >= size) {
            offsetOut = it->first;
            const uint64_t rest = it->second - size;
            freeLba_.erase(it);
            if (rest > 0) {
                freeLba_.emplace(offsetOut + size, rest);
            }
            return true;
        }
    }
    return false;
}

void HybmNvmeSegment::FreeLbaRange(uint64_t offset, uint64_t size)
{
    freeLba_[offset] = size;
    /* coalesce with neighbours */
    auto it = freeLba_.find(offset);
    if (it != freeLba_.begin()) {
        auto prev = std::prev(it);
        if (prev->first + prev->second == offset) {
            prev->second += size;
            freeLba_.erase(it);
            it = prev;
        }
    }
    auto next = std::next(it);
    if (next != freeLba_.end() && it->first + it->second == next->first) {
        it->second += next->second;
        freeLba_.erase(next);
    }
}

void HybmNvmeSegment::FillExtent(NvmeExportInfo &info) const
{
    const auto nqn = SpdkNvmfAdminClient::GetNqn();
    const auto traddr = SpdkNvmfAdminClient::GetTraddr();
    (void)strncpy(info.nqn, nqn.c_str(), sizeof(info.nqn) - 1);
    (void)strncpy(info.traddr, traddr.c_str(), sizeof(info.traddr) - 1);
    info.trsvcid = SpdkNvmfAdminClient::GetTrsvcid();
}

Result HybmNvmeSegment::AllocLocalMemory(uint64_t size, MemSlicePtr &slice) noexcept
{
    if ((size % HYBM_LARGE_PAGE_SIZE) != 0) {
        BM_LOG_ERROR("nvme alloc size not 2M aligned: " << size);
        return BM_INVALID_PARAM;
    }
    const uint64_t slotOffset = allocatedSize_;
    if (slotOffset + size > options_.maxSize) {
        BM_LOG_ERROR("nvme alloc exceeds the rank slot window: " << slotOffset + size << " > " << options_.maxSize);
        return BM_INVALID_PARAM;
    }
    const uint64_t lbaBytes = ((size + granularity_ - 1) / granularity_) * granularity_;
    uint64_t lbaOffset = 0;
    if (!AllocLbaRange(lbaBytes, lbaOffset)) {
        BM_LOG_ERROR("nvme alloc: no contiguous LBA range of " << lbaBytes << " bytes free");
        return BM_ERROR;
    }
    uint32_t nsid = 0;
    auto ret = admin_->AddNs(lbaOffset + lbaBaseEnv_, lbaBytes, nsid);
    if (ret != BM_OK) {
        FreeLbaRange(lbaOffset, lbaBytes);
        return ret;
    }

    const uint64_t gva = reinterpret_cast<uint64_t>(globalVirtualAddress_) +
                         static_cast<uint64_t>(options_.maxSize) * options_.rankId + slotOffset;
    auto realSlice = std::make_shared<MemSlice>(sliceCount_++, HYBM_MEM_TYPE_NVME, MEM_PT_TYPE_SVM, gva, 0, size);
    ret = HybmVaManager::GetInstance().AddVaInfo({{gva, 0, 0}, size, HYBM_MEM_TYPE_NVME}, options_.rankId, true);
    if (ret != BM_OK) {
        BM_LOG_ERROR("nvme alloc: add va info failed, gva: " << VaToStr(gva));
        (void)admin_->RemoveNs(nsid);
        FreeLbaRange(lbaOffset, lbaBytes);
        return ret;
    }

    NvmeExportInfo info{};
    info.gva = gva;
    info.sliceIndex = realSlice->index_;
    info.ownerRank = options_.rankId;
    info.blockId = nextBlockId_++;
    info.nsid = nsid;
    info.lbaBase = (lbaBaseEnv_ + lbaOffset) / 512ULL;
    info.lbaLen = lbaBytes / 512ULL;
    info.vsize = size;
    FillExtent(info);

    slices_.emplace(realSlice->index_, MemSliceStatus{realSlice});
    sliceExtents_.emplace(realSlice->index_, info);
    exportMap_.emplace(realSlice->index_, std::string(reinterpret_cast<const char *>(&info), sizeof(info)));

    HybmNvmeExtent extent{};
    extent.gvaBase = info.gva;
    extent.size = info.vsize;
    extent.ownerRank = info.ownerRank;
    extent.blockId = info.blockId;
    extent.nsid = info.nsid;
    extent.trsvcid = info.trsvcid;
    extent.lbaBase = info.lbaBase;
    extent.lbaLen = info.lbaLen;
    (void)memcpy(extent.nqn, info.nqn, sizeof(extent.nqn));
    (void)memcpy(extent.traddr, info.traddr, sizeof(extent.traddr));
    HybmNvmeExtentRegistry::GetInstance().Register(extent);

    allocatedSize_ += size;
    slice = std::move(realSlice);
    BM_LOG_INFO("nvme extent allocated, gva: " << VaToStr(gva) << " size: " << size << " lba: " << info.lbaBase
                                               << "+" << info.lbaLen << " nsid: " << nsid
                                               << " blockId: " << info.blockId);
    return BM_OK;
}

Result HybmNvmeSegment::RegisterMemory(const void *addr, uint64_t size, MemSlicePtr &slice) noexcept
{
    (void)addr;
    (void)size;
    (void)slice;
    BM_LOG_ERROR("nvme segment does not support user memory registration");
    return BM_NOT_SUPPORTED;
}

Result HybmNvmeSegment::ReleaseSliceMemory(const MemSlicePtr &slice) noexcept
{
    if (slice == nullptr) {
        return BM_INVALID_PARAM;
    }
    auto it = sliceExtents_.find(static_cast<uint32_t>(slice->index_));
    if (it == sliceExtents_.end()) {
        BM_LOG_WARN("nvme release: slice " << slice->index_ << " not owned by this segment");
        return BM_INVALID_PARAM;
    }
    const auto info = it->second;
    (void)admin_->RemoveNs(info.nsid);
    FreeLbaRange((info.lbaBase * 512ULL) - lbaBaseEnv_, info.lbaLen * 512ULL);
    HybmNvmeExtentRegistry::GetInstance().UnregisterByGva(info.gva);
    HybmVaManager::GetInstance().RemoveOneVaInfo(info.gva, HVM_GVA);
    slices_.erase(static_cast<uint32_t>(slice->index_));
    sliceExtents_.erase(it);
    exportMap_.erase(static_cast<uint32_t>(slice->index_));
    BM_LOG_INFO("nvme extent released, gva: " << VaToStr(info.gva) << " nsid: " << info.nsid);
    return BM_OK;
}

Result HybmNvmeSegment::Export(std::string &exInfo) noexcept
{
    (void)exInfo; /* segment-level export carries nothing (mirrors the vmm segment) */
    return BM_OK;
}

Result HybmNvmeSegment::Export(const MemSlicePtr &slice, std::string &exInfo) noexcept
{
    if (slice == nullptr) {
        return BM_INVALID_PARAM;
    }
    auto it = exportMap_.find(static_cast<uint32_t>(slice->index_));
    if (it == exportMap_.end()) {
        BM_LOG_ERROR("nvme export: slice " << slice->index_ << " not found");
        return BM_INVALID_PARAM;
    }
    exInfo = it->second;
    return BM_OK;
}

Result HybmNvmeSegment::GetExportSliceSize(size_t &size) noexcept
{
    size = UNIFIED_EXCHANGE_SEG_INFO_SIZE;
    return BM_OK;
}

Result HybmNvmeSegment::Import(const std::vector<std::string> &allExInfo, void *addresses[]) noexcept
{
    for (size_t i = 0; i < allExInfo.size(); i++) {
        if (allExInfo[i].size() != sizeof(NvmeExportInfo)) {
            BM_LOG_ERROR("nvme import: info size " << allExInfo[i].size() << " != " << sizeof(NvmeExportInfo));
            return BM_INVALID_PARAM;
        }
        NvmeExportInfo info;
        (void)memcpy(&info, allExInfo[i].data(), sizeof(info));
        if (!IsNvmeSlice(info.magic)) {
            BM_LOG_ERROR("nvme import: magic mismatch: " << VaToStr(info.magic));
            return BM_INVALID_PARAM;
        }
        auto ret = HybmVaManager::GetInstance().AddVaInfoFromExternal(
            {{info.gva, 0, 0}, info.vsize, HYBM_MEM_TYPE_NVME}, options_.rankId, info.ownerRank);
        if (ret != BM_OK) {
            BM_LOG_ERROR("nvme import: add external va failed, gva: " << VaToStr(info.gva) << " ret: " << ret);
            return ret;
        }

        HybmNvmeExtent extent{};
        extent.gvaBase = info.gva;
        extent.size = info.vsize;
        extent.ownerRank = info.ownerRank;
        extent.blockId = info.blockId;
        extent.nsid = info.nsid;
        extent.trsvcid = info.trsvcid;
        extent.lbaBase = info.lbaBase;
        extent.lbaLen = info.lbaLen;
        (void)memcpy(extent.nqn, info.nqn, sizeof(extent.nqn));
        (void)memcpy(extent.traddr, info.traddr, sizeof(extent.traddr));
        HybmNvmeExtentRegistry::GetInstance().Register(extent);

        imports_.push_back(info);
        if (addresses != nullptr) {
            addresses[i] = reinterpret_cast<void *>(info.gva);
        }
        BM_LOG_INFO("nvme extent imported, owner: " << info.ownerRank << " gva: " << VaToStr(info.gva)
                                                    << " nsid: " << info.nsid);
    }
    return BM_OK;
}

Result HybmNvmeSegment::RemoveImported(const std::vector<uint32_t> &ranks) noexcept
{
    for (auto it = imports_.begin(); it != imports_.end();) {
        bool drop = false;
        for (const auto rank : ranks) {
            if (it->ownerRank == rank) {
                HybmNvmeExtentRegistry::GetInstance().UnregisterByGva(it->gva);
                HybmVaManager::GetInstance().RemoveOneVaInfo(it->gva, HVM_GVA);
                drop = true;
                break;
            }
        }
        it = drop ? imports_.erase(it) : std::next(it);
    }
    return BM_OK;
}

Result HybmNvmeSegment::Mmap() noexcept
{
    return BM_OK; /* nothing to map: member-side extents were registered at Import time */
}

Result HybmNvmeSegment::Unmap() noexcept
{
    return BM_OK;
}

MemSlicePtr HybmNvmeSegment::GetMemSlice(hybm_mem_slice_t slice, bool quiet) const noexcept
{
    auto index = MemSlice::GetIndexFrom(slice);
    auto it = slices_.find(static_cast<uint32_t>(index));
    if (it == slices_.end()) {
        if (!quiet) {
            BM_LOG_ERROR("nvme segment: slice index " << index << " not found");
        }
        return nullptr;
    }
    return it->second.slice;
}

bool HybmNvmeSegment::MemoryInRange(const void *begin, uint64_t size) const noexcept
{
    if (globalVirtualAddress_ == nullptr) {
        return false;
    }
    auto addr = reinterpret_cast<const uint8_t *>(begin);
    return addr >= globalVirtualAddress_ && (addr + size) <= (globalVirtualAddress_ + totalVirtualSize_);
}
} // namespace mf
} // namespace ock
