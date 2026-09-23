/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
*/
#include "hybm_conn_based_segment.h"

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <sched.h>
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "hybm_logger.h"
#include "hybm_ex_info_transfer.h"
#include "hybm_va_manager.h"
#include "dl_hal_api.h"

using namespace ock::mf;

#ifndef SYS_move_pages
#define SYS_move_pages 239 // aarch64 asm-generic (__NR_move_pages)
#endif

// Pool DRAM NUMA affinity: on multi-socket hosts the pool pages drawn at fault time
// may land on a NUMA node remote from the NPU, degrading DMA bandwidth (measured
// -6% H2D / -19% D2H on a 2-socket 910B node). Three best-effort layers, all optional:
// L1 mbind PREFERRED/BIND on the pool VMA (BIND is banned and PREFERRED is silently
// ignored on some vendor kernels, e.g. EulerOS 5.10.0-*); L2 pin the touching thread
// to a CPU of the NPU-local node for the pool first-touch; L3 migrate already-faulted
// pages to the target node via move_pages, then verify placement by sampling.
// UAPI values from linux/mempolicy.h.
namespace {
constexpr int POOL_NUMA_AUTO = -1;
constexpr int POOL_NUMA_OFF = -2;
constexpr int MPOL_PREFERRED_LOCAL = 1;
constexpr int MPOL_BIND_LOCAL = 2;

int ReadIntFile(const char *path)
{
    std::ifstream f(path);
    int v = 0;
    if (!(f >> v)) {
        return std::numeric_limits<int>::min();
    }
    return v;
}

std::vector<std::pair<int, int>> ParseCpuRanges(const std::string &cpulist)
{
    std::vector<std::pair<int, int>> ranges;
    std::istringstream ss(cpulist);
    std::string item;
    while (std::getline(ss, item, ',')) {
        auto dash = item.find('-');
        try {
            if (dash == std::string::npos) {
                int cpu = std::stoi(item);
                ranges.emplace_back(cpu, cpu);
            } else {
                ranges.emplace_back(std::stoi(item.substr(0, dash)), std::stoi(item.substr(dash + 1)));
            }
        } catch (const std::exception &) {
            return {};
        }
    }
    return ranges;
}

int CpuToNumaNode(int cpu)
{
    if (cpu < 0) {
        return POOL_NUMA_AUTO;
    }
    DIR *dir = opendir("/sys/devices/system/node");
    if (dir == nullptr) {
        return POOL_NUMA_AUTO;
    }
    struct dirent *ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        if (strncmp(ent->d_name, "node", 4) != 0) {
            continue;
        }
        char path[160];
        if (snprintf(path, sizeof(path), "/sys/devices/system/node/%s/cpulist", ent->d_name) <= 0) {
            continue;
        }
        std::ifstream f(path);
        std::string cpulist;
        if (!(f >> cpulist)) {
            continue;
        }
        for (const auto &range : ParseCpuRanges(cpulist)) {
            if (cpu >= range.first && cpu <= range.second) {
                closedir(dir);
                return atoi(ent->d_name + 4);
            }
        }
    }
    closedir(dir);
    return POOL_NUMA_AUTO;
}

// L2 detection: parse "npu-smi info -t topo" -- the row "NPU<x> ... <a-b>" gives the
// NPU's CPU affinity range; map its first CPU to a NUMA node via node*/cpulist.
int DetectViaNpuSmiTopo(uint32_t logicDeviceId)
{
    FILE *pipe = popen("npu-smi info -t topo 2>/dev/null", "r");
    if (pipe == nullptr) {
        return POOL_NUMA_AUTO;
    }
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe) != nullptr) {
        out += buf;
    }
    (void)pclose(pipe);

    const std::string want = "NPU" + std::to_string(logicDeviceId);
    std::istringstream lines(out);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream tokens(line);
        std::string first;
        std::string last;
        std::string tok;
        if (!(tokens >> first) || first != want) {
            continue;
        }
        while (tokens >> tok) {
            last = tok;
        }
        auto dash = last.find('-');
        int cpu = -1;
        try {
            cpu = (dash == std::string::npos) ? (last.empty() ? -1 : std::stoi(last))
                                              : std::stoi(last.substr(0, dash));
        } catch (const std::exception &) {
            continue;
        }
        return CpuToNumaNode(cpu);
    }
    return POOL_NUMA_AUTO;
}

int DetectNpuNumaNode(uint32_t logicDeviceId)
{
    char path[128];
    if (snprintf(path, sizeof(path), "/sys/class/davinci_devices/device%u/numa_node", logicDeviceId) > 0) {
        auto node = ReadIntFile(path);
        if (node >= 0) {
            return node;
        }
    }
    return DetectViaNpuSmiTopo(logicDeviceId);
}

uint64_t NodeFreeHugepages(int node)
{
    char path[192];
    if (snprintf(path, sizeof(path), "/sys/devices/system/node/node%d/hugepages/hugepages-2048kB/free_hugepages",
                 node) <= 0) {
        return 0;
    }
    auto v = ReadIntFile(path);
    return (v == std::numeric_limits<int>::min()) ? 0 : static_cast<uint64_t>(v);
}

int PoolNumaTargetNode(uint32_t logicDeviceId)
{
    // MF_POOL_NUMA_NODE: -1 auto-detect (default), -2 disabled, >=0 forced node id
    const char *env = getenv("MF_POOL_NUMA_NODE");
    if (env != nullptr) {
        int v = atoi(env);
        if (v == POOL_NUMA_OFF || v >= 0) {
            return v;
        }
    }
    return DetectNpuNumaNode(logicDeviceId);
}

void BindPoolVmaNuma(void *addr, uint64_t size, bool hugepage, uint32_t logicDeviceId)
{
    auto node = PoolNumaTargetNode(logicDeviceId);
    if (node == POOL_NUMA_OFF) {
        return;
    }
    if (node == POOL_NUMA_AUTO) {
        BM_LOG_INFO("pool numa affinity: NPU numa node not detectable, skipped; "
                    "set MF_POOL_NUMA_NODE to force a node");
        return;
    }
    int mode = MPOL_BIND_LOCAL;
    if (hugepage) {
        auto need = (size + HYBM_LARGE_PAGE_SIZE - 1) / HYBM_LARGE_PAGE_SIZE;
        if (NodeFreeHugepages(node) < need) {
            mode = MPOL_PREFERRED_LOCAL;
            BM_LOG_WARN("pool numa affinity: free 2MB hugepages on node " << node << " < needed " << need
                        << ", falling back to MPOL_PREFERRED (placement NOT guaranteed; reserve via"
                        " /sys/devices/system/node/node" << node
                        << "/hugepages/hugepages-2048kB/nr_hugepages)");
        }
    }
    unsigned long nodemask = 1UL << node;
    auto mbindCall = [&addr, &size, &nodemask, node](int m) {
        errno = 0;
        return syscall(SYS_mbind, addr, size, static_cast<unsigned int>(m), &nodemask,
                       static_cast<unsigned long>(node + 1), 0UL);
    };
    long rc = mbindCall(mode);
    if (rc != 0 && errno == EINVAL && mode == MPOL_BIND_LOCAL) {
        // Observed on EulerOS vendor kernels (e.g. 5.10.0-*.euleros*): MPOL_BIND is
        // rejected kernel-wide while MPOL_PREFERRED works. Fall back to best-effort.
        mode = MPOL_PREFERRED_LOCAL;
        rc = mbindCall(mode);
        if (rc == 0) {
            struct utsname uts;
            std::string release = (uname(&uts) == 0) ? uts.release : "unknown";
            BM_LOG_INFO("pool numa affinity: kernel " << release
                        << " rejects MPOL_BIND, using MPOL_PREFERRED (best-effort placement)");
        }
    }
    if (rc != 0) {
        BM_LOG_WARN("pool numa affinity: mbind(addr:" << addr << " size:" << size << " node:" << node
                    << " mode:" << mode << ") failed: " << errno << ", " << SafeStrError(errno));
    } else {
        BM_LOG_INFO("pool numa affinity: pool VMA bound to node " << node << " (mode " << mode << ")");
    }
}

// Layer 2: pin the calling thread to a CPU of the target node BEFORE any pool page is
// faulted, so both our first-touch and the device registration (GUP) faults land on the
// NPU-local node. Required on vendor kernels where mbind is ignored at fault time
// (verified EulerOS 5.10.0-*). Returns the pinned CPU or -1 on failure.
int PinThreadToNodeCpu(int node, cpu_set_t *saved)
{
    if (node < 0 || saved == nullptr) {
        return -1;
    }
    char path[128];
    if (snprintf(path, sizeof(path), "/sys/devices/system/node/node%d/cpulist", node) <= 0) {
        return -1;
    }
    std::ifstream f(path);
    std::string cpulist;
    if (!(f >> cpulist)) {
        return -1;
    }
    auto ranges = ParseCpuRanges(cpulist);
    if (ranges.empty() || ranges.front().first < 0 || ranges.front().first >= CPU_SETSIZE) {
        return -1;
    }
    if (sched_getaffinity(0, sizeof(*saved), saved) != 0) {
        return -1;
    }
    cpu_set_t target;
    CPU_ZERO(&target);
    CPU_SET(ranges.front().first, &target);
    if (sched_setaffinity(0, sizeof(target), &target) != 0) {
        return -1;
    }
    return ranges.front().first;
}

void RestoreThreadAffinity(const cpu_set_t *saved)
{
    if (saved != nullptr) {
        (void)sched_setaffinity(0, sizeof(*saved), saved);
    }
}

// Restores the thread affinity mask on scope exit, including early returns.
struct AffinityGuard {
    cpu_set_t saved{};
    bool armed{false};
    ~AffinityGuard()
    {
        if (armed) {
            RestoreThreadAffinity(&saved);
        }
    }
};

// Migrate pages to node via move_pages. Returns 0 on success, -1 on global syscall
// failure (errno set), otherwise the number of pages reported not moved. Per-page
// errno is written into status.
long MovePagesToNode(std::vector<void *> &addrs, int node, std::vector<int> &status)
{
    status.assign(addrs.size(), 0);
    if (addrs.empty()) {
        return 0;
    }
    std::vector<int> nodes(addrs.size(), node);
    errno = 0;
    long rc = syscall(SYS_move_pages, 0UL, static_cast<unsigned long>(addrs.size()), addrs.data(),
                      nodes.data(), status.data(), 0UL);
    if (rc == -1) {
        return -1;
    }
    size_t stuck = 0;
    for (auto s : status) {
        if (s < 0) {
            stuck++;
        }
    }
    return static_cast<long>(stuck);
}

// Query current node of pages via move_pages (nodes == NULL). Returns 0 or -1 (errno).
long QueryPagesNode(std::vector<void *> &addrs, std::vector<int> &status)
{
    status.assign(addrs.size(), 0);
    if (addrs.empty()) {
        return 0;
    }
    errno = 0;
    return syscall(SYS_move_pages, 0UL, static_cast<unsigned long>(addrs.size()), addrs.data(), nullptr,
                   status.data(), 0UL);
}

// Layer 3: post-fault migration. Fixes pages faulted by others (e.g. device
// registration pinning) regardless of fault-time policy. Chunked, one retry pass.
void MigratePoolPagesNuma(void *addr, uint64_t size, uint64_t pageSize, int node)
{
    const uint64_t chunkPages = 256;
    const uint64_t totalPages = (size + pageSize - 1) / pageSize;
    std::vector<void *> addrs;
    std::vector<void *> failed;
    uint64_t migrated = 0;
    for (uint64_t base = 0; base < totalPages; base += chunkPages) {
        uint64_t n = std::min(chunkPages, totalPages - base);
        addrs.clear();
        addrs.reserve(n);
        for (uint64_t i = 0; i < n; i++) {
            addrs.push_back(static_cast<char *>(addr) + (base + i) * pageSize);
        }
        std::vector<int> status;
        if (MovePagesToNode(addrs, node, status) == -1) {
            BM_LOG_WARN("pool numa affinity: move_pages failed: errno " << errno << ", "
                        << SafeStrError(errno) << " (node " << node << ", pages " << addrs.size() << ")");
            return;
        }
        for (size_t i = 0; i < addrs.size(); i++) {
            if (status[i] < 0) {
                failed.push_back(addrs[i]);
            } else {
                migrated++;
            }
        }
    }
    if (!failed.empty()) {
        std::vector<int> status;
        if (MovePagesToNode(failed, node, status) != -1) {
            size_t stuck = 0;
            for (auto s : status) {
                if (s < 0) {
                    stuck++;
                }
            }
            migrated += failed.size() - stuck;
            if (stuck > 0) {
                BM_LOG_WARN("pool numa affinity: " << stuck << "/" << totalPages
                            << " pool pages could not move to node " << node);
            }
        }
    }
    BM_LOG_INFO("pool numa affinity: migrated " << migrated << "/" << totalPages
                << " pool pages to node " << node);
}

// Placement oracle: sample up to 32 pages via move_pages query mode (numa_maps lacks
// hugetlb entries on some vendor kernels), log per-node distribution, WARN if off target.
void LogPoolNumaPlacement(void *addr, uint64_t size, uint64_t pageSize, int targetNode, const char *phase)
{
    const uint64_t totalPages = (size + pageSize - 1) / pageSize;
    const uint64_t samples = std::min<uint64_t>(totalPages, 32);
    const uint64_t step = std::max<uint64_t>(totalPages / samples, 1);
    std::vector<void *> addrs;
    for (uint64_t i = 0; i < totalPages && addrs.size() < samples; i += step) {
        addrs.push_back(static_cast<char *>(addr) + i * pageSize);
    }
    std::vector<int> status;
    if (QueryPagesNode(addrs, status) == -1) {
        BM_LOG_WARN("pool numa placement (" << phase << "): move_pages query failed: errno " << errno
                    << ", " << SafeStrError(errno));
        return;
    }
    std::vector<std::pair<int, uint64_t>> dist;
    uint64_t onTarget = 0;
    uint64_t valid = 0;
    for (auto s : status) {
        if (s < 0) {
            continue;
        }
        valid++;
        if (s == targetNode) {
            onTarget++;
        }
        bool found = false;
        for (auto &kv : dist) {
            if (kv.first == s) {
                kv.second++;
                found = true;
                break;
            }
        }
        if (!found) {
            dist.emplace_back(s, 1);
        }
    }
    std::sort(dist.begin(), dist.end());
    std::string placement;
    for (auto &kv : dist) {
        if (!placement.empty()) {
            placement += " ";
        }
        placement += "N" + std::to_string(kv.first) + "=" + std::to_string(kv.second);
    }
    if (targetNode >= 0 && valid > 0 && onTarget == valid) {
        BM_LOG_INFO("pool numa placement (" << phase << "): " << placement << " (target node "
                    << targetNode << ", sampled " << addrs.size() << "/" << totalPages << " pages)");
    } else {
        BM_LOG_WARN("pool numa placement (" << phase << "): "
                    << (placement.empty() ? "no resident pages" : placement)
                    << " -- expected node " << targetNode << ", on-target " << onTarget << "/" << valid
                    << " sampled pages; pool is NOT NPU-local");
    }
}
} // namespace

Result HybmConnBasedSegment::ValidateOptions() noexcept
{
    if (options_.segType != HYBM_MST_DRAM || options_.maxSize == 0 || (options_.maxSize % HYBM_LARGE_PAGE_SIZE) != 0) {
        BM_LOG_ERROR("Validate options error type(" << options_.segType << ") size(" << options_.maxSize);
        return BM_INVALID_PARAM;
    }

    if (UINT64_MAX / options_.maxSize < options_.rankCnt) {
        BM_LOG_ERROR("Validate options error rankCnt(" << options_.rankCnt << ") size(" << options_.maxSize);
        return BM_INVALID_PARAM;
    }

    return BM_OK;
}

Result HybmConnBasedSegment::ReserveMemorySpace(void **address) noexcept
{
    BM_ASSERT_LOG_AND_RETURN(ValidateOptions() == BM_OK, "Failed to validate options.", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(globalVirtualAddress_ == nullptr, "Already prepare virtual memory.", BM_NOT_INITIALIZED);
    BM_ASSERT_LOG_AND_RETURN(address != nullptr, "Invalid param, address is NULL.", BM_INVALID_PARAM);
    BM_ASSERT_LOG_AND_RETURN(PrepareShareMemoryFd() == BM_OK, "PrepareShareMemoryFd failed.", BM_ERROR);
    BM_ASSERT_LOG_AND_RETURN(options_.rankId < options_.rankCnt,
                             "rank(" << options_.rankId << ") but total " << options_.rankCnt, BM_INVALID_PARAM);

    uint64_t totalSize = options_.rankCnt * options_.maxSize;
    uint64_t localSize = options_.enable56BitsGva ? options_.maxSize : totalSize;
    auto gvaInfo = HybmVaManager::GetInstance().AllocReserveGva(options_.rankId, totalSize, localSize,
                                                                HYBM_MEM_TYPE_HOST, options_.enable56BitsGva);
    BM_ASSERT_LOG_AND_RETURN(gvaInfo.va[HVM_GVA] > 0, "Invalid param, start is 0.", BM_ERROR);
    void *startAddr = reinterpret_cast<void *>(gvaInfo.va[HVM_GVA]);
    if (!options_.enable56BitsGva) {
        void *mapped = mmap(startAddr, totalSize, PROT_NONE,
                            MAP_FIXED_NOREPLACE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_PRIVATE, -1, 0);

        if (mapped == MAP_FAILED || (uint64_t)mapped != (uint64_t)startAddr) {
            BM_LOG_ERROR("Failed to mmap size:" << totalSize << " addr:" << startAddr << " ret:" << mapped
                                                << " error: " << errno);
            return BM_ERROR;
        }
    }
    globalVirtualAddress_ = (uint8_t *)startAddr;
    totalVirtualSize_ = totalSize;
    if (options_.enable56BitsGva) {
        localVirtualBase_ = (uint8_t *)gvaInfo.va[HVM_DVA];
    } else {
        localVirtualBase_ = globalVirtualAddress_ + options_.maxSize * options_.rankId;
    }
    allocatedSize_ = 0UL;
    sliceCount_ = 0;
    *address = globalVirtualAddress_;
    return BM_OK;
}

Result HybmConnBasedSegment::UnReserveMemorySpace() noexcept
{
    BM_LOG_INFO("un-reserve memory space.");
    FreeMemory();
    return BM_OK;
}

void HybmConnBasedSegment::LvaShmReservePhysicalMemory(void *mappedAddress, uint64_t size) noexcept
{
    BM_ASSERT_RET_VOID(mappedAddress != nullptr);
    auto *pos = static_cast<uint8_t *>(mappedAddress);
    uint64_t setLength = 0;
    while (setLength < size) {
        *pos = 0;
        setLength += HYBM_LARGE_PAGE_SIZE;
        pos += HYBM_LARGE_PAGE_SIZE;
    }

    pos = static_cast<uint8_t *>(mappedAddress) + (size - 1L);
    *pos = 0;
}

Result HybmConnBasedSegment::AllocLocalMemory(uint64_t size, MemSlicePtr &slice) noexcept
{
    if ((size % HYBM_LARGE_PAGE_SIZE) != 0UL || size + allocatedSize_ > options_.maxSize) {
        BM_LOG_ERROR("invalid allocate memory size : " << size << ", now used " << allocatedSize_ << " of "
                                                       << options_.maxSize);
        return BM_INVALID_PARAM;
    }

    void *sliceAddr = localVirtualBase_ + allocatedSize_;
    auto gva = reinterpret_cast<uint64_t>(globalVirtualAddress_ + options_.maxSize * options_.rankId + allocatedSize_);
    void *mapped = nullptr;
    MemAllocMethod allocMethod = MemAllocMethod::MMAP;
    auto ret = MapSlice(mapped, sliceAddr, allocatedSize_, size, gva, allocMethod);
    if (ret != BM_OK) {
        return ret;
    }
    allocatedSize_ += size;
    slice = std::make_shared<MemSlice>(sliceCount_++, HYBM_MEM_TYPE_HOST, MEM_PT_TYPE_SVM, gva,
                                       reinterpret_cast<uint64_t>(mapped), size, allocMethod);
    slices_.emplace(slice->index_, slice);
    BM_LOG_DEBUG("allocate slice(idx:" << slice->index_ << ", size:" << slice->size_ << " va:" << mapped << ").");
    return BM_OK;
}

Result HybmConnBasedSegment::Export(std::string &exInfo) noexcept
{
    return BM_OK;
}

Result HybmConnBasedSegment::Export(const MemSlicePtr &slice, std::string &exInfo) noexcept
{
    if (slice == nullptr) {
        BM_LOG_ERROR("input slice is nullptr");
        return BM_INVALID_PARAM;
    }

    auto pos = slices_.find(slice->index_);
    if (pos == slices_.end()) {
        BM_LOG_ERROR("input slice(idx:" << slice->index_ << ") not exist.");
        return BM_INVALID_PARAM;
    }

    if (pos->second.slice != slice) {
        BM_LOG_ERROR("input slice(magic:" << std::hex << slice->magic_ << ") not match.");
        return BM_INVALID_PARAM;
    }

    auto exp = exportMap_.find(slice->index_);
    if (exp != exportMap_.end()) {
        exInfo = exp->second;
        return BM_OK;
    }
    AllocatedGvaInfo gvaInfo{};
    if (slice->size_ > 0) {
        bool found = false;
        std::tie(gvaInfo, found) = HybmVaManager::GetInstance().FindAllocByVa(slice->vAddress_, HVM_HVA);
        if (!found) {
            BM_LOG_ERROR("input host va(" << slice->vAddress_ << ") not match.");
            return BM_INVALID_PARAM;
        }
    }

    HostExportInfo info;
    info.gva = gvaInfo.base.va[HVM_GVA];
    info.sliceIndex = static_cast<uint32_t>(slice->index_);
    info.rankId = options_.rankId;
    info.size = slice->size_;
    info.pageTblType = MEM_PT_TYPE_SVM;
    info.memSegType = HYBM_MST_DRAM;
    info.exchangeType = HYBM_INFO_EXG_IN_NODE;
    auto ret = LiteralExInfoTranslater<HostExportInfo>{}.Serialize(info, exInfo);
    if (ret != BM_OK) {
        BM_LOG_ERROR("export info failed: " << ret);
        return BM_ERROR;
    }

    exportMap_[slice->index_] = exInfo;
    return BM_OK;
}

Result HybmConnBasedSegment::Import(const std::vector<std::string> &allExInfo, void *addresses[]) noexcept
{
    LiteralExInfoTranslater<HostExportInfo> translator;
    std::vector<HostExportInfo> deserializedInfos{allExInfo.size()};
    for (auto i = 0U; i < allExInfo.size(); i++) {
        auto ret = translator.Deserialize(allExInfo[i], deserializedInfos[i]);
        if (ret != 0) {
            BM_LOG_ERROR("deserialize imported info(" << i << ") failed.");
            return BM_INVALID_PARAM;
        }
        if (addresses != nullptr) {
            addresses[i] = reinterpret_cast<void *>(deserializedInfos[i].gva);
        }
    }

    try {
        std::copy(deserializedInfos.begin(), deserializedInfos.end(), std::back_inserter(imports_));
    } catch (...) {
        BM_LOG_ERROR("copy failed.");
        return BM_MALLOC_FAILED;
    }
    return BM_OK;
}

Result HybmConnBasedSegment::Mmap() noexcept
{
    for (const auto &import : imports_) {
        if (import.rankId == options_.rankId) {
            continue;
        }
        mappedGvaMem_.insert(import.gva);

        auto ret = HybmVaManager::GetInstance().AddVaInfoFromExternal(
            {import.gva, 0, 0, import.size, HYBM_MEM_TYPE_HOST}, options_.rankId, import.rankId);
        BM_ASSERT_RETURN(ret == BM_OK, ret);
    }
    imports_.clear();
    return 0;
}

Result HybmConnBasedSegment::Unmap() noexcept
{
    for (auto gva : mappedGvaMem_) {
        HybmVaManager::GetInstance().RemoveOneVaInfo(gva);
    }
    mappedGvaMem_.clear();
    return 0;
}

MemSlicePtr HybmConnBasedSegment::GetMemSlice(hybm_mem_slice_t slice, bool quiet) const noexcept
{
    auto index = MemSlice::GetIndexFrom(slice);
    auto pos = slices_.find(index);
    if (pos == slices_.end()) {
        if (quiet) {
            BM_LOG_DEBUG("Failed to get slice, index(" << index << ") not find");
        } else {
            BM_LOG_ERROR("Failed to get slice, index(" << index << ") not find");
        }
        return nullptr;
    }

    auto target = pos->second.slice;
    if (!target->ValidateId(slice)) {
        if (quiet) {
            BM_LOG_DEBUG("Failed to get slice, slice is invalid index(" << index << ")");
        } else {
            BM_LOG_ERROR("Failed to get slice, slice is invalid index(" << index << ")");
        }
        return nullptr;
    }

    return target;
}

bool HybmConnBasedSegment::MemoryInRange(const void *begin, uint64_t size) const noexcept
{
    if (begin < globalVirtualAddress_) {
        return false;
    }

    if (reinterpret_cast<const uint8_t *>(begin) + size > globalVirtualAddress_ + totalVirtualSize_) {
        return false;
    }

    return true;
}

void HybmConnBasedSegment::FreeMemory() noexcept
{
    while (!slices_.empty()) {
        auto slice = slices_.begin()->second.slice;
        // Only pool slices own backing memory; user-registered slices point to caller-owned HVA.
        const bool ownsBackingMemory = (slice->gva_ != 0U);
        ReleaseSliceMemory(slice);
        if (ownsBackingMemory) {
            FreeAllocatedMemory(reinterpret_cast<void *>(slice->vAddress_), slice->size_, slice->allocMethod_);
        }
    }
    Unmap();

    if (localVirtualBase_ != nullptr && allocatedSize_ > 0) {
        // All slice memory has been freed via FreeAllocatedMemory
        // No need to munmap localVirtualBase_ as each slice's memory is released individually
        localVirtualBase_ = nullptr;
    }

    if (options_.enable56BitsGva) {
        globalVirtualAddress_ = localVirtualBase_ = nullptr;
    } else if (globalVirtualAddress_ != nullptr) {
        if (munmap(globalVirtualAddress_, totalVirtualSize_) != 0) {
            BM_LOG_ERROR("Failed to unmap global memory");
        }
        HybmVaManager::GetInstance().FreeReserveGva((uintptr_t)globalVirtualAddress_);
        globalVirtualAddress_ = nullptr;
    }
}

Result HybmConnBasedSegment::PrepareShareMemoryFd() const noexcept
{
    if (options_.shmFd < 0) {
        return BM_OK;
    }

    struct stat buf{};
    auto ret = fstat(options_.shmFd, &buf);
    if (ret != 0) {
        BM_LOG_ERROR("share mem fd: " << options_.shmFd << " stat failed: " << errno << ":" << strerror(errno));
        return BM_INVALID_PARAM;
    }

    if (static_cast<uint64_t>(buf.st_size) >= options_.size) {
        return BM_OK;
    }

    ret = ftruncate(options_.shmFd, static_cast<off_t>(options_.size));
    if (ret != 0) {
        BM_LOG_ERROR("share mem fd: " << options_.shmFd << " truncate from " << buf.st_size << " to " << options_.size
                                      << " failed: " << errno << ":" << strerror(errno));
        return BM_ERROR;
    }

    return BM_OK;
}

Result HybmConnBasedSegment::MapSlice(void *&mapped, void *sliceAddr, uint64_t lvOffset, uint64_t size,
                                      uint64_t gva, MemAllocMethod &allocMethod) noexcept
{
    if (size == 0) {
        return BM_OK;
    }

    void *dva = nullptr;
    uint64_t pageSize = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
    mapped = AllocMemory(sliceAddr, lvOffset, size, allocMethod, pageSize);
    if (mapped == MAP_FAILED) {
        BM_LOG_ERROR("Failed to alloc size:" << size << " addr:" << sliceAddr << " mapped:" << mapped
                                             << " error:" << errno << ", " << SafeStrError(errno));
        return BM_ERROR;
    }

    // Layer 2: pin to a CPU of the NPU-local node BEFORE any page is faulted -- both our
    // first-touch and the device registration (GUP) faults must land on the local node.
    // Pages already registered with the device are GUP-pinned and can never be migrated,
    // so fault-time placement is the last controllable moment.
    int numaNode = PoolNumaTargetNode(logicDeviceId_);
    bool numaWork = numaNode >= 0 && allocMethod == MemAllocMethod::MMAP;
    AffinityGuard affinityGuard;
    if (numaWork) {
        int cpu = PinThreadToNodeCpu(numaNode, &affinityGuard.saved);
        affinityGuard.armed = (cpu >= 0);
        BM_LOG_INFO("pool numa affinity: node " << numaNode << " first-touch "
                    << (cpu >= 0 ? "pinned to cpu " + std::to_string(cpu)
                                 : std::string("pin failed, placement not guaranteed")));
    }

    LvaShmReservePhysicalMemory(mapped, size);

    if (options_.dataOpType & HYBM_DOP_TYPE_DEVICE_RDMA) {
        auto ret = DlHalApi::HalHostRegister(mapped, size, HOST_MEM_MAP_DEV, logicDeviceId_, &dva);
        if (ret != BM_OK) {
            BM_LOG_ERROR("register host va failed, ret:" << ret);
            FreeAllocatedMemory(mapped, size, allocMethod);
            return BM_ERROR;
        }
    }
    if (affinityGuard.armed) {
        RestoreThreadAffinity(&affinityGuard.saved);
        affinityGuard.armed = false;
    }
    int ret = HybmVaManager::GetInstance().AddVaInfo(
        {gva, (uint64_t)dva, (uint64_t)mapped, size, HYBM_MEM_TYPE_HOST}, options_.rankId);
    if (ret != 0) {
        BM_LOG_ERROR("AddVaInfo failed, size: " << size << " ret: " << ret);
        if (options_.dataOpType & HYBM_DOP_TYPE_DEVICE_RDMA) {
            DlHalApi::HalHostUnregisterEx(mapped, logicDeviceId_, HOST_MEM_MAP_DEV);
        }
        FreeAllocatedMemory(mapped, size, allocMethod);
        return ret;
    }

    if (numaWork) {
        // Evidence before/after migration; L3 only helps pages NOT pinned by the device.
        LogPoolNumaPlacement(mapped, size, pageSize, numaNode, "after-register");
        MigratePoolPagesNuma(mapped, size, pageSize, numaNode);
        LogPoolNumaPlacement(mapped, size, pageSize, numaNode, "after-migrate");
    }
    return BM_OK;
}

void* HybmConnBasedSegment::AllocMemory(void *sliceAddr, uint64_t lvOffset, uint64_t size,
                                        MemAllocMethod &allocMethod, uint64_t &pageSize)
{
    void* mapped;
    auto prot = PROT_READ | PROT_WRITE;
    int mmapFd = options_.shmFd < 0 ? -1 : options_.shmFd;
    int mmapFlags = options_.shmFd < 0 ? (MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE) : (MAP_FIXED | MAP_SHARED);
    uint64_t mmapOffset = options_.shmFd < 0 ? 0 : lvOffset;

    // 1. Try to alloc DRAM with hugepage via mmap
    mapped = mmap(sliceAddr, size, prot, mmapFlags | MAP_HUGETLB, mmapFd, mmapOffset);
    if (mapped == sliceAddr) {
        BM_LOG_INFO("Successfully allocated " << size << " bytes DRAM hugepage via mmap. addr:" << mapped);
        BindPoolVmaNuma(mapped, size, true, logicDeviceId_);
        allocMethod = MemAllocMethod::MMAP;
        pageSize = HYBM_LARGE_PAGE_SIZE;
        return mapped;
    }
    BM_LOG_WARN("Failed to alloc size:" << size << " with hugepage via mmap, error: " << errno << ", "
        << SafeStrError(errno) << ". Use 'grep -i huge /proc/meminfo' to check hugepages, "
        "and use 'echo <page_num> > /proc/sys/vm/nr_hugepages' to set hugepages.");

    // 2. try to alloc DRAM with hugepage via halMemAlloc
    if (options_.enable56BitsGva && options_.shmFd < 0) {
        BM_LOG_WARN("Trying halMemAlloc for DRAM hugepage allocation. " << "size:" << size);

        // Use halMemAlloc to allocate DRAM huge page memory on host
        // Flag: MEM_HOST (host memory) | MEM_TYPE_DDR (DDR/DRAM) | MEM_PAGE_HUGE (2MB huge page)
        uint64_t allocFlag = MEM_HOST | MEM_TYPE_DDR | MEM_PAGE_HUGE;
        void *halAllocPtr = nullptr;

        int ret = DlHalApi::HalMemAlloc(&halAllocPtr, size, allocFlag);
        if (ret != 0 || halAllocPtr == nullptr) {
            BM_LOG_WARN("halMemAlloc failed, ret:" << ret << " ptr:" << halAllocPtr << ". Cannot allocate " << size
                                                   << " bytes DRAM huge page memory");
        } else {
            allocMethod = MemAllocMethod::HAL_MEM_ALLOC;
            pageSize = HYBM_LARGE_PAGE_SIZE;
            BM_LOG_INFO("Successfully allocated DRAM hugepage via halMemAlloc. "
                        "addr:" << halAllocPtr << " size:" << size);
            return halAllocPtr;
        }
    }

    // 3. try to alloc DRAM with 4k page via mmap
    mapped = mmap(sliceAddr, size, prot, mmapFlags, mmapFd, mmapOffset);
    if (mapped == sliceAddr) {
        BM_LOG_INFO("Successfully allocated " << size << " bytes DRAM 4K page via mmap. addr:" << mapped);
        BindPoolVmaNuma(mapped, size, false, logicDeviceId_);
        allocMethod = MemAllocMethod::MMAP;
        pageSize = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
        return mapped;
    }

    return MAP_FAILED;
}

Result HybmConnBasedSegment::RemoveImported(const std::vector<uint32_t> &ranks) noexcept
{
    for (auto &rank : ranks) {
        if (rank >= options_.rankCnt) {
            BM_LOG_ERROR("input rank is invalid! rank:" << rank << " rankSize:" << options_.rankCnt);
            return BM_INVALID_PARAM;
        }
    }
    for (const auto rank : ranks) {
        uint64_t gvaLocal = reinterpret_cast<uint64_t>(globalVirtualAddress_) + options_.maxSize * rank;
        auto it = mappedGvaMem_.lower_bound(gvaLocal);
        auto st = it;
        while (it != mappedGvaMem_.end() && (*it) < gvaLocal + options_.maxSize) {
            HybmVaManager::GetInstance().RemoveOneVaInfo(*it);
            ++it;
        }
        if (st != it) {
            mappedGvaMem_.erase(st, it);
        }
    }

    // remove imports_ infos for specified ranks
    imports_.erase(std::remove_if(imports_.begin(), imports_.end(),
                                  [&ranks](const HostExportInfo &info) {
                                      return std::find(ranks.begin(), ranks.end(), info.rankId) != ranks.end();
                                  }),
                   imports_.end());
    return BM_OK;
}

Result HybmConnBasedSegment::RegisterMemory(const void *addr, uint64_t size, MemSlicePtr &slice) noexcept
{
    auto ret = RegisterMemCommon(addr, size, slice);
    BM_ASSERT_RETURN(ret == BM_OK, ret);
    slices_.emplace(slice->index_, slice);
    return BM_OK;
}

Result HybmConnBasedSegment::ReleaseSliceMemory(const MemSlicePtr &slice) noexcept
{
    if (slice == nullptr) {
        BM_LOG_ERROR("input slice is nullptr");
        return BM_INVALID_PARAM;
    }

    auto pos = slices_.find(slice->index_);
    if (pos == slices_.end()) {
        BM_LOG_ERROR("input slice(idx:" << slice->index_ << ") not exist.");
        return BM_INVALID_PARAM;
    }

    if (pos->second.slice != slice) {
        BM_LOG_ERROR("input slice(magic:" << std::hex << slice->magic_ << ") not match.");
        return BM_INVALID_PARAM;
    }

    HybmVaManager::GetInstance().RemoveOneVaInfo(slice->vAddress_, HVM_HVA);
    slices_.erase(pos);

#if defined(ASCEND_NPU)
    /* symmetric with RegisterMemCommon: only host-dram slices register a hal mapping
     * (HalHostRegister); hbm slices never do, so their release must not unregister either.
     * Same guard as HybmVmmBasedSegment::ReleaseSliceMemory. */
    const bool needUnregister = (options_.dataOpType & HYBM_DOP_TYPE_DEVICE_RDMA) != 0U &&
                                slice->memType_ == HYBM_MEM_TYPE_HOST;
    if (needUnregister) {
        auto unregRet = DlHalApi::HalHostUnregisterEx(reinterpret_cast<void *>(slice->vAddress_),
                                                      logicDeviceId_, HOST_MEM_MAP_DEV);
        if (unregRet != 0) {
            BM_LOG_ERROR("HalHostUnregisterEx failed, idx:" << slice->index_
                         << " ret:" << unregRet << "; teardown continues");
        }
    }
#endif

    return BM_OK;
}

void HybmConnBasedSegment::FreeAllocatedMemory(void *ptr, uint64_t size, MemAllocMethod allocMethod) noexcept
{
    if (ptr == nullptr || ptr == MAP_FAILED) {
        return;
    }

    if (allocMethod == MemAllocMethod::HAL_MEM_ALLOC) {
        int ret = DlHalApi::HalMemFree(ptr);
        if (ret != 0) {
            BM_LOG_ERROR("Failed to free memory allocated by HalMemAlloc, ptr:" << ptr << " ret:" << ret);
        } else {
            BM_LOG_INFO("Successfully freed memory via HalMemFree, ptr:" << ptr << " size:" << size);
        }
    } else {
        if (munmap(ptr, size) != 0) {
            BM_LOG_ERROR("Failed to munmap memory, ptr:" << ptr << " size:" << size << " error:" << errno);
        } else {
            BM_LOG_INFO("Successfully freed memory via munmap, ptr:" << ptr << " size:" << size);
        }
    }
}

Result HybmConnBasedSegment::GetExportSliceSize(size_t &size) noexcept
{
    size = sizeof(HostExportInfo);
    return BM_OK;
}
