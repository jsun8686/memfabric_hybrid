# NVMe-oF 远端 NVMe SSD 池管理设计方案

> 状态：定稿（2026-10-15）。本文档为 `docs` 下归档的设计基线，实施按第 12 节阶段计划推进；
> 历史讨论中的中间形态（FAR staging 块缓存、ralloc 层 block API、ralloc 层 admin 模块）均已废弃，
> 以本文档的归属口径为准。

## 1. 背景与目标

当前池管理介质仅支持 HOST DRAM 与 NPU HBM。本设计新增第三介质 **NVMe SSD**（经 NVMe-oF，
SPDK 双端直连形态），补齐冷层容量：

- **易失语义**：与 DRAM 池一致——池回收/客户端退出即数据作废，无 checkpoint、无跨池身份；
- **容量与带宽目标**：单 FAR 节点多盘聚合，数十 TB、端到端 20GB/s+；
- **写语义**：NVMe CQE = 已落盘，库层不加 flush API 面。

## 2. 用户形态（统一 API，GVA 编址不变）

用户接口与现有 DRAM/HBM 池完全一致，SPDK 交互全部内化：

```c
ralloc.create(..., dataOpType |= NVME, maxNvmeSize=...)   /* NVMe 能力与窗口声明 */
extend_remote_mem(h, MEM_TYPE_NVME, 64TB, &info)          /* info.gva 照常返回(追加 lba/nqn 字段) */
smem_ralloc_copy(h, src, nvmeGva + off, len, flags)       /* 写 → 落 SSD */
smem_ralloc_copy(h, nvmeGva + off, dst, len, flags)       /* 读 → 出 SSD */
smem_ralloc_copy_batch(...)                               /* NVMe 段/内存段混合, 自动分流 */
smem_ralloc_wait(h)                                       /* 覆盖 NVMe 在途 */
```

- 用户按**字节粒度任意编址** NVMe 块，512B 扇区对全在库内消化（未对齐头尾 RMW bounce）；
- NVMe 块是组内"第一公民"：与 DRAM/HBM 块同级的第三介质块，**全员可见可访问**；
- **AICore 边界**：`device_copy/v2/v3` 对 NVMe GVA 一律 `SM_NOT_SUPPORTED`（SSD 无 MR，
  物理不可达）。冷层工作流 = `copy(nvmeGva→dramGva)` 预取 + 现有 v3 gather，两步均为标准接口。

## 3. 总体架构（SPDK 双端直连，host RDMA 网卡）

```
                        配置面(本地 JSON-RPC, 零数据)        控制面(复用 ralloc RPC)
 NEAR 节点(每卡一进程)                                        FAR 节点
┌─────────────────────┐                                      ┌───────────────────────────┐
│ torch 进程           │   PLACEMENT ──► master(候选表+NVMe列)│ memfabric daemon(元数据)   │
│  smem 层(控制薄壳)   │◄─ JOIN_ALLOC ─► executor(校验放开)──│  └─JSON-RPC─┐             │
│  hybm 层             │        └ GroupUpdate 广播 extent ──► │             ▼             │
│   HostDataOpNvme     │             (全员导入, 屏障后回包)     │  nvmf_tgt(独立进程)      │
│    └ dlopen libspdk  ╪══ NVMe-oF over RDMA(单跳) ══════════╪══ tgt NIC ◄─ bdev(raid0)   │
│  pinned 池 + SDMA 腿 │        host NIC ◄────────────────────              └─ 本地 NVMe×N │
└─────────────────────┘                                      └───────────────────────────┘
```

- **网卡**：双端 host PCIe NIC + rdma-core/ibverbs 用户态栈。现有数据面的 libra 私有栈与
  NPU device RoCE 均不参与（SPDK 用不了 libra，需标准 verbs provider——P0 甄别项）。
  host NIC 与 device NIC 是两张卡，存储面与现有 DEVICE_RDMA 数据面硬件上互不抢带宽；
- **daemon 不碰数据**：数据面单网络跳，tgt CPU 零接触数据；tgt 视角 = 合法多 initiator
  （每个 NEAR 进程一条独立连接，NVMe-oF 天然多主机）；
- **SDMA 仅在 NEAR 内部最后一跳**：host NIC 与 NPU HBM 分属两个 DMA 域（host IOMMU vs
  设备 IOMMU），物理上不可合并，只能 SDMA 跨 PCIe 落 HBM。

## 4. 分层归属【定版核心：与既有 import/export 架构对称】

现有块生命周期骨架（`SmemRallocEntry::ExtendLocalMem`，smem_ralloc_entry.cpp）：

```
① hybm_alloc_local_memory(entity, memType, size)  → MemSegment::AllocLocalMemory（本地物化）
② hybm_export(entity, slice, 0, &info)            → MemSegment::Export（序列化交换信息, string）
③ globalGroup_->GroupUpdate()                     → 组屏障（CAS 版本推进）
④ 成员侧 MemSegment::Import + Mmap                → 导入
```

NVMe 对称落位：**新增第四种 MemSegment**，SPDK 双触点全部收敛在 hybm 层，smem 层零 SPDK 感知。

### 4.1 HybmNvmeSegment（第四种 MemSegment，`HYBM_MST_NVME`）

| MemSegment 虚方法 | 内存段（DRAM/HBM） | **NVMe 段** |
|---|---|---|
| `ReserveMemorySpace` | VMM/大页预留 | VaManager NVMe 带预留（`AllocReserveGva`，永不物化） |
| `AllocLocalMemory` | mmap/显存分配 | **LBA 位图切区间 + JSON-RPC `nvmf_subsystem_add_ns` + `add_host` 白名单**（FAR 的 SPDK 触点） |
| `Export(slice, exInfo)` | 序列化 GVA/键 | 序列化 `{gva, lba, nqn, traddr, trsvcid, nsid}`（string 载体，无跨版本结构 ABI 问题） |
| `Import(allExInfo)` | 建映射/注册 | 解析 extent → `AddVaInfoFromExternal(onlyGva)` + 登记 `HostDataOpNvme` extent 表（成员侧导入） |
| `Mmap/Unmap` | 建解本地映射 | **no-op**（连接懒化到首 IO） |
| `ReleaseSliceMemory` | 释放内存 | JSON-RPC `remove_ns` + 位图回收 |
| `GetMemoryType/MemoryInRange` | — | NVMe 带判断 |
| `RegisterMemory` | 用户 MR 注册 | 不支持（返回 NOT_SUPPORTED） |

### 4.2 HostDataOpNvme（数据通路，对等 `hybm_data_op_device_rdma.cpp`）

- 类形态样板 = `hybm_data_op_host_shm.cpp`（Initialize/IsSupportedDirection/DataCopy/
  DataCopyAsync/BatchDataCopy/Wait）；
- compose 层 `GetPrioritedDataOperators` 对等加档（`HYBM_DOP_TYPE_NVME` 位，能力协商走
  `GetRank2RankOpType` 实体 tag 交换）；
- Initialize/UnInitialize：dlopen libspdk（新 `dl_spdk_api.h/.cpp`，libra 先例）+ env 动态
  注册模式 + **懒初始化**（纯 DRAM/HBM 池进程零成本）；
- DataCopy/Async：extent 表查 `{nqn,nsid,lbaBase}` → pinned bounce → `spdk_nvme_ns_cmd_read/write`；
  LOCAL_DEVICE 端点补 SDMA 腿（`aclrtMemcpyAsync` 挂用户流）；双缓冲流水；
- BatchDataCopy：同方向段多 qpair 并行；
- Wait：**按需轮询**（提交线程 wait 期间 `process_completions`，无常驻 poller 线程）+ stream 同步；
- 方向码支持 4 个新方向（见 §11），其余拒绝。

### 4.3 smem/ralloc 层（纯控制薄壳）

- executor `OnJoinAlloc`：现有 extend 分支几乎原样（`existEntry->ExtendLocalMem(NVME, size)`
  走 ①~④ 骨架），仅改校验放开 + create 分支 `options.nvmeVASpace`；
- entry `ExtendLocalMem`：memType 映射加一档，骨架一行不改；
- master：候选表第三列 `nvmeCommittedBytes` 簿记（零 SPDK 交互），负载 = 已提交字节 +
  在途 grant（乐观账本复用）；
- 混合批次分流复用 smem/bm 层既有逐段分类点（NVMe 段自然分流出方向码）。

## 5. GVA 分配与映射

```
分配(两级, 复用现有机制):
① 池创建: AllocReserveGva(rank, maxNvmeSize, localSize=0, NVMe, 56bit?)  ← 纯 VA 预留
   （"预留大、物化小"语义现有代码已支持；64TB 也只是 VA 数字）
   maxNvmeSize 进池定义一致性比对（同 maxDram/maxHbm，组布局必须一致）
② extend: FAR 在自己 rank 槽位 NVMe 带内取 [gvaBase, gvaBase+size)（2M 对齐管理粒度）
   ← 块登记在属主 rank 带内, GetRankIdByGva 对全成员统一生效

映射(两层, 一次查表 + 一次算术):
第一层: allocatedMap_[HVM_GVA] 查找(O(log n), 即 GetGvaMemType 现路径)
        → memType=NVMe → ClassifyAddress=GLOBAL_NVME(新 AddrType)
        → COPY_DIRECTION_TABLE 4×4 扩 5×5 → InferCopyDirection 自动吐出新方向码
        → compose 分发到 HostDataOpNvme
第二层: extent 线性仿射  startLba = lbaBase + (off >> 9),  numLbas = len >> 9
        （无页表无多级间接; 未对齐头尾 RMW bounce）
```

两表分工：VaManager 只答"这段 GVA 是 NVMe 媒质、归哪 rank"（供分类/方向推导）；
extent 明细表（nqn/nsid/lbaBase）在 `HostDataOpNvme` 内，由 NVMe 段的 Import 供给（同层闭环）。

## 6. 组内 import/export

```
extend_remote_mem(NVME) 全流程:
① placement → JOIN_ALLOC → FAR executor（现有路径）
② FAR: HybmNvmeSegment::AllocLocalMemory = LBA 位图切区间 + JSON-RPC 签发
   {nqn, nsid, lbaBase, lbaLen} + 白名单 add_host（hostnqn 按 rank 派生:
   nqn.2016-06.io.spdk:memfabric-pool<id>-rank<r>）
   → 实体 GroupUpdate 广播 extent（Export 格式）→ 屏障: 全员导入完成才回包
③ 各成员: NVMe 段 Import → AddVaInfoFromExternal(onlyGva=true, importedRankId=FAR)
   + HostDataOpNvme extent 表登记（与远端内存块导入语义同构: 远端内存块本也不本地 mmap）
④ extend 返回 = 任何成员立即可用该 gva（不变式与 DRAM 块完全一致）
```

- 白名单随组事件维护：join → `add_host`，rank-down → `remove_host`（段/group 事件路径，hybm 层）；
- 各成员 ctrlr 池按 `(nqn, traddr, trsvcid)` 懒连接（首 IO 才 `spdk_nvme_connect`）；
- 一致性等级与内存块相同（无硬件 coherence，应用负责）；NVMe 写以扇区为原子单位，
  多写者冲突窗口比 RDMA 大——README 边界注明，应用层版本/分区兜底。

## 7. 数据路径与性能预算

```
读(swap-in): SSD ─PCIe DMA(gen4 ~7GB/s/盘, 多盘聚合)→ FAR tgt buffer
             ─RDMA WRITE(~23GB/s)→ NEAR NIC ─DMA→ NEAR pinned DRAM
             ─SDMA(~20-25, 挂流/双缓冲流水)→ NPU HBM
写(eager backup): 镜像反向; NVMe CQE = 已落盘
端到端(大块流式, 双缓冲流水后) ≈ min(NVMe 聚合, RDMA, SDMA) ≈ 18-20GB/s
小 IO ~百 us 级; 子 4K/未对齐: pinned 池 RMW bounce; 同名调用性能量级不同——文档化
```

## 8. SPDK 交互边界

| 端 | 交互形态 | 代码层 | 载体 |
|---|---|---|---|
| NEAR/成员侧（数据面） | 进程内库调用（dlopen libspdk，`spdk_nvme_*` C API） | hybm 层 | `HostDataOpNvme` + `dl_spdk_api` |
| FAR 资源端（控制面） | **进程间 JSON-RPC**（本地 unix socket → 独立 nvmf_tgt 进程） | hybm 层 | `HybmNvmeSegment::AllocLocalMemory/Release` 内嵌 admin client |
| master 侧 | 零 SPDK 交互 | smem 层 | `smem_ralloc_master.cpp` 第三列 |

FAR 端 SPDK RPC 方法 × 生命周期：

| 触发时机 | SPDK 现成 RPC 方法 |
|---|---|
| daemon 启动 `--nvme`（健康探测+容量读取） | `framework_get_rpc_methods` / `bdev_get_bdevs` |
| 部署脚本（一次性，systemd，daemon 不 spawn tgt） | `bdev_nvme_attach_controller`、`bdev_raid_create`、`nvmf_create_transport(RDMA)`、`nvmf_create_subsystem`、`nvmf_subsystem_add_listener(traddr:4420)` |
| JOIN_ALLOC 签发 extent | `nvmf_subsystem_add_ns` |
| 成员 join/rank-down | `nvmf_subsystem_add_host` / `remove_host` |
| 池回收/易失失效 | `nvmf_subsystem_remove_ns` |
| tgt 探活失败 | RPC 超时 → 容量上报清零 → master 自动停止放置 |

SPDK 的 MR 只覆盖 NEAR pinned host DRAM，永远看不到 HBM/GVA——两域间只有库调度 SDMA。

## 9. 资源与多进程策略（一节点 8 进程）

| 项 | 对策 |
|---|---|
| CPU | 按需轮询无常驻线程；懒初始化（纯内存池进程零成本） |
| 内存 | 库属 pinned 池受控（`MF_NVME_BUF_MB`，THP/大页后备，**池一次注册长期持有**，动态注册模式）；`MF_NVME_QPAIRS` 默认 2 |
| 连接 | 8 进程 = 8 独立 initiator（NVMe-oF 天然多主机）；tgt 限额内 |
| 动态注册性能 | 稳态 0~2%（注册一次性 ~ms/GB@THP，SGE/TLB 噪声级）；P0 量化，>5% 则 sidecar 顶上 |
| 兜底 | 每节点单 sidecar initiator（shm 环，数据零多一跳），API 面不变 |

多进程冲突面：uverbs 多进程打开安全；EAL 每进程独立；争抢点仅 hugepage 池/memlock/轮询核
（对策见表）；tgt 侧 8×连接在配置上限内，故障时有 8×重连风暴（可接受）。

## 10. 生命周期、故障与一致性

- 池回收/客户端退出 → `remove_ns` → extent 回收；**ns 销毁即断链**（stale qpair 读失败），
  陈旧 extent 表条目安全退化；无 checkpoint；
- FAR tgt 不健康 → admin RPC 超时 → 容量上报清零 → 后续 extend 自动重放置
  （rank-down watch/退避复用）；
- NEAR qpair 断链/ns 消失 → SM_* → group event → 上层重放置；
- NVMe 写完成语义 = NVMe CQE（SPDK 保证），库不加 flush 层。

## 11. ABI 变更清单（全部 append-only，旧二进制互操作安全）

| 触点 | 变更 |
|---|---|
| `smem_ralloc_def.h` | `SMEM_RALLOC_MEM_TYPE_NVME`、`SMEMRA_DATA_OP_NVME`、`mem_info_t` 追加 `{blockId, lbaBase, lbaLen, nqn[], traddr[], trsvcid, nsid}` |
| `smem_ralloc.h/.cpp` | extend 校验放宽（IO 512B 粒度、管理 2M 对齐）+ NVMe 分支 |
| hybm 公共头 | `HYBM_DOP_TYPE_NVME`、`HYBM_MEM_TYPE_NVME`、`MemSegType` 加 `HYBM_MST_NVME`、`MemSegmentOptions.maxNvmeSize`、`hybm_options.nvmeVASpace`、方向枚举追加 4 个（`LOCAL_HOST_TO_GLOBAL_NVME` / `GLOBAL_NVME_TO_LOCAL_HOST` / `LOCAL_DEVICE_TO_GLOBAL_NVME` / `GLOBAL_NVME_TO_LOCAL_DEVICE`） |
| `hybm_va_manager` | `GLOBAL_NVME` AddrType、`COPY_DIRECTION_TABLE` 扩 5×5（现有 op 的 default 分支天然拒绝未知方向） |
| RPC msg | `nvmeCommittedBytes` + nvmf 三元组回传字段 |
| 校验面 | NVMe 媒质/地址进 `device_copy/v2/v3` → `SM_NOT_SUPPORTED` |

## 12. 阶段计划与验收

| 阶段 | 内容 | 验收 |
|---|---|---|
| **P0** | 甄别实验（集群执行）：① host NIC rdma-core provider 可用性（libra≠SPDK 可用）② host NIC 实测 RDMA 带宽 ③ torch 进程内嵌动态注册模式（THP/4K 注册耗时与稳态 BW 量化）④ 同节点 8 进程并发 ⑤ AIO 文件 bdev 全链路连通；代码：ABI/枚举/映射/校验 + 互操作 UT | 甄别报告 + UT 绿 |
| **P1** | FAR：tgt 部署脚本 + `HybmNvmeSegment`（admin client + LBA 位图 + Export/Import）+ 白名单随组维护 + master 第三列 + executor 校验放开 | extend 返回三元组，第二成员可见可用 |
| **P2** | `HostDataOpNvme` 全量 + compose 加档 + VaManager NVMe 类 + `dl_spdk_api` + extent 导入端 + ctrlr 懒连接池 + per-op UT + pymf | 单/批量读写正确性 + BW 矩阵 + 混合批次 |
| **P3** | 示例 `16_nvme_ssd_pool` + README + ex15 冷层接入 | 统一 API 全流程 + 多盘聚合 20GB/s+ + 多成员访问 + DRAM/HBM 池零回归 |

## 13. 风险与回退

| 风险 | 回退 |
|---|---|
| host NIC 无 rdma-core provider（libra 私有栈 ≠ SPDK 可用） | 内核栈变体（nvmet/内核 initiator），API 面不变 |
| DPDK 侵入 torch / 动态注册不干净（>5% 损耗） | 每节点 sidecar initiator |
| 存储面/数据面共卡争抢 | trsvcid 4420 + VLAN 隔离；host NIC 与 device NIC 硬件本就互不抢 |
| 旧部署无 SPDK | dlopen 可选加载（libra 先例），优雅 NOT_SUPPORTED；CMake/Bazel 不硬链接 |
| 多写者冲突窗口 | 应用层版本/分区（README 边界），与池一致性等级声明一致 |

## 14. 对等新增文件清单

| 文件 | 对等样板 |
|---|---|
| `src/hybm/csrc/mm/hybm_nvme_segment.cpp/.h` | `hybm_vmm_based_segment.cpp`（物化→签发、Export/Import 对称） |
| `src/hybm/csrc/data_operation/host/hybm_data_op_nvme.cpp/.h` | `hybm_data_op_host_shm.cpp`（类形态）+ `hybm_data_op_device_rdma.cpp`（传输复杂度） |
| `src/hybm/csrc/under_api/dl_spdk_api.h/.cpp` | `dl_hccp_api.cpp`（dlopen libra 模式 → dlopen libspdk_nvme/env_dpdk） |
| `test/ut/testcase/hybm/mm/hybm_nvme_segment_test.cpp` | `hybm_vmm_based_segment_test.cpp`（AIO 文件 bdev，无需真盘） |
| `test/ut/testcase/hybm/data_operator/hybm_data_op_nvme_test.cpp` | 既有 per-op UT（mock loader） |
| 触点 | compose 加档(~6 行)、方向枚举/VaManager 扩类、两层 op-bit 映射、`smem_ralloc_def.h/.h`、`smem_ralloc.cpp`、executor/master、`pymf_hybrid.cpp`、`examples/memory_pool/02_scale_out/16_nvme_ssd_pool/` |
