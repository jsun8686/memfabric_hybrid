# NVMe-oF 远端 NVMe SSD 池管理设计方案

> 状态：**已实施（批 1-3 完成，2026-10-20）**。定稿于 2026-10-15；
> 批 1（ABI 骨架 + HybmNvmeSegment，UT 4/4）、批 2（dl_nvme_api + HostDataOpNvme + compose/pymf，
> UT 7/7 + 23/23）、批 3（example 16 + 文档）均已落地；
> 无盘模拟联调（malloc bdev + NVMe/TCP loopback）通过，identify 连通性验证 OK（P0 项：
> SPDK TCP initiator 可用性通过）。
> 历史讨论中的中间形态（FAR staging 块缓存、ralloc 层 block API、ralloc 层 admin 模块）均已废弃，
> 以本文档的归属口径为准。实施偏差记录：
> - RPC 报文为 v3（160B，仅追加 `maxNvmeSize`/`nvmeCommittedBytes` 两控制面字段），
>   `smem_ralloc_mem_info_t` 保持 `{rankId, gva}` 两字段（NVMe 细节走进程级 extent 注册表，不进用户 API）；
> - 数据面 loader 命名为 `dl_nvme_api`（设计稿为 `dl_spdk_api`）；
> - 联调发现并已修复两个真 bug：transport id `adrfam` 必须显式 `IPv4(1)`（0=INVALID 被拒）、
>   单命令必须按控制器 MDTS（nvmf TCP 默认 128KB）拆分（SPDK 不代拆）；
> - 实施后重构（2026-10-20）：NVMe op 位按协议显式化为 `NVME_TCP(1<<7)/NVME_RDMA(1<<8)`
>   （替换原 `NVME` 位，同一算子类、位决定协议，与 host 面 HOST_RDMA/HOST_TCP 惯例对称；
>   两位互斥）。协议随 create/JOIN_ALLOC 控制面传到 FAR，FAR 在 NVMe 段预留时查询 target
>   实际监听 transport（nvmf_get_transports）做一致性校验，不一致阻断；协议唯一真源是
>   op 位（曾存在的 `MF_NVME_TRSTRING` env 已删除，避免打破声明/校验闭环）；
> - 进程级全流程首跑（2026-10-20，sim loopback）又暴露管理面签发链三个协议适配 bug，已修复：
>   `bdev_split_create` 的切分大小参数是 `split_size_mb`（MB 单位，原误发扇区数 `split_size` 被拒）；
>   split bdev 命名从 `n1` 起（原按 `n0` 拼接错位）；split 表须按全盘容量一次建齐
>   （`bdev_get_bdevs` 查几何，各 extent 引用连续段，原误发本 extent 的 count）。
>   附带修复：非法参数被盲容忍后 target 断链导致下一条 RPC EPIPE（现仅容忍 "exist" 类错误）、
>   `NvmeEnvU64` 不识别 `64M` 类后缀（曾被解析为 64 字节再钳到 2M）、
>   失败重试路径 GVA 带泄漏（entity 重初始化前先释放旧段）。
>   随后二轮：`bdev_raid_create` 的 JSON-RPC 键是 `level`（未知键致服务端解码失败 -32700，
>   原误发 `raid_level`；单 split extent 现直接以 split bdev 为 ns 后端、跳过聚合层）、
>   executor JOIN_ALLOC 的 Initialize 失败分支补 `UnInitialize()`（此前每轮重试泄漏一条
>   GVA 带，重试轮地址递增即其表征）。
>   终局（rpc.py -v 实证）：split bdev 命名 `<base>p<i>` 从 0 起（前述 n1 起亦误）；
>   `nvmf_subsystem_add_ns` 的 `bdev_name` 须嵌套在 `namespace` 子对象内（平铺被
>   decode 拒），成功响应为裸数字 `"result":<nsid>`；`nvmf_subsystem_remove_ns` 保持
>   平铺 `{nqn, nsid}`。失败回滚最终形态：SmemRallocEntry::Initialize 失败自包含回滚
>   （逐 slice 释放 + unreserve + destroy entity），替代早先在 executor/InitNvmeSegment
>   打的补丁。

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

## 15. 实施联调记录（持续追加）

### 联调终态（2026-10-21，commit 12cd6b9/bb9157b）

**sim 双形态数据面全通，均含 64MB round-trip / 子扇区 RMW / MDTS 拆分 BW 矩阵 / 干净退出**：

| 形态 | 拓扑 | 传输 | 峰值 BW |
|---|---|---|---|
| 单机无盘模拟 | 单容器（33） | NVMe/TCP loopback | 0.02-0.03 GB/s（正确性验证） |
| **跨机 RDMA 模拟盘** | FAR 33（192.168.200.10, mlx5）↔ NEAR 35（192.168.200.11, mlx5） | NVMe/RoCE 真实网卡 | **3.37 GB/s** |

控制面跨机（store/group 走管理口 10.90.22.x）、FAR 侧协议一致性校验（op bit ↔ target transport）、NEAR 角色本地无 target 的降级路径（DEBUG 化）均验证。UT：HybmDataOpNvme 8/8（含 opts 前缀偏移/trtype/KAT=0 哨兵断言）。

### dlopen 宿主依赖链（SPDK 25.05 实证）

SPDK 共享库（`--with-shared` 构建）的部分运行时依赖**不进 DT_NEEDED**（原生程序如 `nvmf_tgt` 靠链接期全家桶），dlopen 宿主必须自行以 `RTLD_NOW|RTLD_GLOBAL` 预载注入全局符号表，否则 `undefined symbol`。实证四环，`dl_nvme_api.cpp` 的 `PreloadDeps` 已内置：

| 环 | 符号 | 库 | 来源 |
|---|---|---|---|
| 1 | `EVP_MD_CTX_free` | `libcrypto.so.3/.1.1` | 系统包（OpenSSL 1.1+/3.x） |
| 2 | `crc16_t10dif` | `libisal.so.2/.so` | SPDK 默认把 isa-l 静态链入 target，需 `<spdk>/isa-l` 子目录手动 `autogen/configure/make` 产共享版并部署到 `build/lib`（SPDK 重编后重做） |
| 3 | `rte_mempool_ops_table` 等 | DPDK 全家 `librte_*.so*` | 目录扫描预载：`MF_NVME_DPDK_LIB_DIR` 显式优先，默认遍历 `LD_LIBRARY_PATH`（部署要求含 `<spdk>/build/lib` 与 `<spdk>/dpdk/build/lib` 双路径） |
| 4 | `OPENSSL_init_ssl` | `libssl.so.3/.1.1` | 系统包 |
| 5 | `sock connection error`（连接从未发起） | `libspdk_sock_posix.so` | sock 实现经 `SPDK_NET_IMPL_REGISTER_DEFAULT` **constructor** 注册进框架注册表并设默认实现（v25.05.x `sock_module.h`）；constructor 仅在库加载时运行，且该库不在任何 DT_NEEDED（应用靠链接期显式带入）。dlopen 宿主不预载则 `spdk_sock_connect_ext` 对空注册表**静默返回 NULL**（`sock.c` 循环零次），内核 `connect()` 从不发起。诊断特征：strace 无对应 connect 系统调用。**顺序约束（实证）**：`libspdk_util` 引用 env 层符号 `spdk_realloc`，其提供者 `libspdk_env_dpdk.so` 未记录在 DT_NEEDED——sock 实现必须在 env 库 dlopen 成功**之后**加载，否则 `undefined symbol: spdk_realloc`；预载失败打 WARN（含 dlerror），不再静默 |

### external-EAL 初始化（SPDK dynamic 注册模式）

`spdk_dma_malloc`/qpair 缓冲依赖活的 DPDK EAL；`spdk_env_dpdk_post_init` 的前提是宿主已自行 `rte_eal_init`。`DlNvmeApi::InitDpdkEnv` 已内置：`dlsym(RTLD_DEFAULT,"rte_eal_init")`（DPDK 全家已预载故符号必在）→ `rte_eal_init("memfabric-nvme","--in-memory")`——**免 hugepage、免共享配置文件**，容器 sim 零依赖；幂等（二次调用 ALREADY 被忽略）；无 DPDK 符号（UT stub 场景）返回 NOT_SUPPORTED 由 `EnsureReady` 容忍。

### Keep-Alive 必须禁用（target 侧 keep alive timeout 断链，实证）

SPDK 默认 `keep_alive_timeout_ms=10000`，库自动发 KAT 的前提是宿主**周期轮询 admin qpair**（官方文档明示）。本库按需只轮询 io qpair → KAT 从未发送 → target 侧 `nvmf_ctrlr_keep_alive_poll` 在 connect 后整 10s 断链（target/client 日志毫秒级吻合），在飞的 IO 报 `CQ transport error -6 (ENXIO)`。周期轮询 admin qpair 不可行（IO 空闲 >10s 仍断链，且需常驻线程违背按需设计），故 connect 时显式 `keep_alive_timeout_ms=0` 禁用 target 端监控。

**opts_size 契约（教训记录）**：nvme.h 明示 `opts_size "Must be set to sizeof(struct spdk_nvme_ctrlr_opts), or 0 if opts is NULL"`——nvme.c 的 `nvme_ctrlr_opts_init` 只在 opts_size 范围内 default 填充与拷贝，**其内部 856B opts 的其余字段保持未初始化**；曾误按 DPDK sock_opts 的"前缀 ABI"模式传 20 字节前缀，导致内部 opts 尾部的 `tls_psk/dhchap_key` 指针域为栈垃圾 → connect 阶段 keyring 解引用 SIGSEGV。正确做法：856 字节不透明 buffer → `spdk_nvme_ctrlr_get_default_ctrlr_opts(buf, 856)` 全量默认 → 按前缀结构偏移写 `keepAliveTimeoutMs=0` → `connect(trid, buf, 856)`；`MfSpdkNvmeCtrlrOptsPrefix`（20B）仅作字段偏移定位，**不得作为 opts_size 传递**。

### SPDK v25.05 initiator ABI 三坑（gdb 栈实证）

| 坑 | 现象与机理 | 对策（已内置） |
|---|---|---|
| `spdk_nvme_connect` 自 24.09 起为 **三参** `(trid, opts, opts_size)` | 旧单参调用把 x1/x2 寄存器残留垃圾当 opts/opts_size，垃圾 opts 尾部的 `tls_psk`/`dhchap_key` 指针进 keyring 解引用 → SIGSEGV（栈顶 `spdk_key_get_name(key=0xffffffff)` ← `nvme_tcp_generate_tls_credentials`） | typedef 三参，调用传 `(trid, nullptr, 0)`（官方"opts 为 NULL 则 opts_size 须 0"路径，零 TLS） |
| `struct spdk_nvme_transport_id` 布局 = `trstring / trtype(4B) / adrfam(4B) / traddr / trsvcid / subnqn / priority`，sizeof=596（char 数组后无对齐垫） | 自声明结构若缺 trtype 或 adrfam 位置在 traddr 之后，我们填的 traddr 字节会被 SPDK 当作 trtype/adrfam 读取 | `MfSpdkNvmeTransportId` 按上游字段序重排 + `static_assert(sizeof==596)`；`GetEndpoint` 显式填 `trtype`（NVMe spec：tcp→3/rdma→1，FC=2 勿混淆——transport 路由按 trstring，trtype 用于日志与内部检查） |
| `spdk_env_dpdk_post_init(bool legacy_mem)` 单 bool 参数 | 多余第二参在 ABI 上无害但属未定义行为边缘 | typedef 对齐单参 |

## 16. 数据面性能优化（NVME_RDMA 窗口引擎，2026-10-23 实施）

### 优化前基线与瓶颈（33↔35 RDMA，BW 矩阵 write/read GB/s）

| 尺寸 | 4K | 64K | 1M | 16M |
|---|---|---|---|---|
| 基线（深度=1 串行） | 0.37 / 0.37 | 2.32 / 2.74 | 2.69 / 3.37 | 1.81 / 2.62 |
| **窗口引擎后（2026-10-23 实测）** | 0.31 / 0.30 | 2.16 / 2.51 | **5.97 / 4.57** | **6.71 / 5.54** |
| 提升 | ~持平 | ~持平 | **2.2× / 1.36×** | **3.7× / 2.1×** |

4K/64K 持平符合设计预期（单请求命令数=1，无窗口收益；批量小 IO 并发是 batch 窗口的事）。TP 打点（404 次 copy）：`STAGE_COPY` 3606 次 TOTAL 51.7ms = `COPY_TOTAL`(103ms) 的 **50%**（staging 双拷贝成为首要瓶颈——B3 零拷贝的靶子，预判兑现）；`POLL_WAIT` 75944 次仅 10.4ms（10%，等待不是瓶颈）；`QPAIR_WAIT` AVG 0.112us（无锁竞争）；`RMW_STAGE_B` 仅 1 次；`PRELOAD` 311ms 一次性（A8a，池创建时）。

read 1M 1.36× 未达 2× 的机理：读路径 stage-out（staging→user）在收割线程串行，流水化程度低于写路径的填窗 stage-in——50% 拷贝占比对读路径伤害更直接，B3 读路径优先。

### 窗口引擎（SubmitWindowed，两轮流水）

- **命令展开**（`BuildCmds`，纯函数）：一次请求（或整个 batch）展开为命令序列——mid 段整扇区直传（cap=min(MDTS, chunk)）、头/尾亚扇区 RMW（round1 读半段共享窗口 → merge → round2 写）；
- **窗口**：填窗（staging 懒获取 + stage-in + 提交）至 `MF_NVME_QDEPTH`（默认 16）→ poll 收割（stage-out/merge/还池）→ 补发，流水重叠取代逐命令等待；错误路径 = 停止补发 + 收割在飞 + 报首错（qpair 死亡则弃单，防死等）；
- **batch 聚合**（对齐 host_rdma 批量模式）：全批次命令共享窗口（4K 小 IO 并发主通道），多 endpoint 按组各占窗口，组失败不阻塞其余组；
- **cpl 状态检查**：`IoDone` 按 v25.05 nvme_spec.h 自声明前缀（16bit status word，`raw & 0x0FFE` 判 SC/SCT）置错。

### 并发模型（qpair 租赁，无全局 IO 锁）

删除 `ioMutex_`。SPDK qpair 非线程安全 → **qpair 即并发边界**：窗口全程独占一个 slot（`try_lock` 轮转，全忙阻塞 = 天然背压），并发上限 = `MF_NVME_QPAIRS`（默认 4）；单线程退化为纯轮转零开销。窗口状态（cmds/scratch/IoCtx）全部栈/局部，slot 锁封闭并发正确性。IoCtx 非原子（SPDK 回调在 poll 线程内联执行，无跨线程竞争）。

### 生命周期前置（懒加载 → 池创建/导入时，fail-fast）

| 阶段 | 动作 | 失败语义 |
|---|---|---|
| 池 create | `LoadExtendLibrary(DL_EXT_LIB_NVME)` dlopen 全家 + op `Initialize` → EAL init | **硬失败**（create 即报"SPDK 环境不可用"） |
| extent 导入/分配 | `HybmNvmeExtentRegistry::Register` 触发 warmup 回调 → `HostDataOpNvme::Warmup`（connect + 预建 qpair slots） | **硬失败**：Import/AllocLocalMemory 报错并自包含回滚（va info/registry/imports_ 全撤） |
| 首 IO | endpoint 必须已存在（`GetEndpoint` 未命中报 `BM_NOT_CONNECTED`，不惰性连接兜底） | 破损装配显式暴露 |

收益：首 IO 尾延迟 ~1.3s（库 1.2s + connect 0.1s）→ 接近 0；部署错误（缺库/断网/协议不符）在 create/JOIN 即暴露。FAR 侧 AllocLocalMemory 的 warmup 为 loopback 连接，语义对称。

### 地址模型注记（TransformVa 空实现的正确性）

用户内存为**原生 HVA/DVA**（不进 GVA 体系）；NVMe GVA 恒为远端盘介质（本地 VA manager 恒无映射）→ 尽力转换恒为空操作，空实现正确（对比 host_rdma：其 GLOBAL_HOST 侧可能映射到本 rank 池块，需要转换）。已在头文件注释固化，防止误判复发。

### 观测（TP 打点 8 个）

`TP_HYBM_NVME_COPY_TOTAL / LOOKUP / STAGE_COPY / SUBMIT / POLL_WAIT / RMW_STAGE_B / QPAIR_WAIT / PRELOAD`（ptracer 机制，与 `TP_HYBM_RDMA_*` 同接入方式）；`QPAIR_WAIT` 观测多线程背压，`POLL_WAIT/STAGE_COPY` 定位拷贝与等待占比。基线协议：client.py 已含 warmup 排除首样本。

### 环境变量（终版）

| 变量 | 默认 | 说明 |
|---|---|---|
| `MF_NVME_QDEPTH` | 16（clamp 1..128） | 窗口深度（单窗在飞命令数） |
| `MF_NVME_QPAIRS` | 4 | slot 数 = 并发窗口上限 |
| `MF_NVME_BUF_MB` | 4 | staging 单段上限（段=min(MDTS, BUF_MB, 尺寸)） |
| `MF_NVME_BUF_SLOTS` | =QDEPTH | 池深（段/扇区两类池各计） |

应急回退位：`MF_NVME_QDEPTH=1 + MF_NVME_QPAIRS=1` 近似旧行为。staging 两级懒池（chunk 块 / 512B 扇区块），活块数 ≤ qdepth。

### 验收结果（2026-10-23 回填）

| 线 | 内容 | 结果 |
|---|---|---|
| UT | 18 用例（warmup 失败回滚/窗口深度/提交错误收割/poll 错误/batch 聚合/双读并发/slot 隔离/env 钳位） | **18/18 绿** + 覆盖率门禁通过；两轮联调各捕获一个真实 bug（BuildCmds 头尾越界、post_init 重入） |
| RDMA 复测（33↔35） | BW 矩阵 + TP 归档 | write 1M 2.2×、16M 3.7×，read 16M 2.1× **达标**；read 1M 1.36× 未达（STAGE_COPY 50% 所致，B3 项）；参数矩阵与 `MF_NVME_QDEPTH/QPAIRS` 调优待 B3 前后对比 |
| 稳定性 | FAR daemon 不重启，NEAR 多轮 create/destroy/BW | **通过**（坑一/坑二修复后；warmup 幂等、endpoint 重复建链、`EnsureSpdkEnv` 闩锁全验证） |
| TCP sim 回归 | 单机 round-trip/RMW/BW | 通过（正确性口径不变） |
| device 方向 | D2H/H2D 原生 DVA 路径 | **待测**（见验证指引，B2 收尾项） |

### 坑：tgt + daemon 同机共存时 EAL PCI 探测冲突（2026-10-23 实证，FAR 不重启二次 create 暴露）

**现象**：FAR daemon（新库）首次 NVMe 路径 warmup 即 `spdk_rdma_utils_create_mem_map: Unable to create memory map` → connect 失败；NEAR 侧（无 tgt 的节点）一切正常。

**根因链**：33 节点 nvmf tgt 常驻占用 RDMA 网卡（tgt 日志 4×`PCI_BUS: cannot be used`）→ daemon 的 `rte_eal_init("--in-memory")` **无 `--no-pci`** → EAL 尝试绑定全部 PCI 设备与 tgt 撞车 → eal_init 真失败被旧代码当"EALREADY"容忍 → `spdk_env_dpdk_post_init` 在半死 EAL 上失败又被 WARN 吞掉 → `g_mem_reg_map == NULL` → 首次 fabric connect 在 `spdk_mem_map_alloc`→`mem_map_notify_walk` 的 `if (!g_mem_reg_map) return -EINVAL`（**SPDK 全路径唯一无日志的失败点**，故现场只见 "Unable to create memory map" 无 ibv_reg_mr 日志）。

**修复（三处）**：① `InitDpdkEnv` argv 加 **`--no-pci`**（我们只用 DPDK 内存子系统，NVMe-oF 走 rdma-core verbs 非 PMD，根本不需要 PCI——tgt/daemon 从此同机无冲突）；② eal_init 负返回**区分 `rte_errno==EALREADY`（良性容忍）与真失败（报错）**；③ `EnsureReady` 将 post_init 失败从 WARN 升级为**硬失败**（A8a 语义补全：环境坏在池 create 即报，而非拖到 connect 报谜语）。旧库时代 FAR 侧懒加载从未触发故未暴露；A8a 把库+EAL 提前到 create 首次点亮此坑。

### 坑二：`spdk_env_dpdk_post_init` 不可重入（daemon 不重启二次 create 暴露）

**现象**：`--no-pci` 修复后第一次 create/warmup/destroy 全通；同 daemon 第二次 create 报 `EAL: The callback is already exist` + `spdk_env_dpdk_post_init: Failed to allocate mem_map`。

**根因**：v25.05 `mem_map_init` 每次都 `rte_mem_event_callback_register("spdk",...)`——DPDK 对同名回调二次注册返回 **EEXIST** → mem_map_init 失败。第一次 pool destroy → op `UnInitialize` 置 `ready_=false` → 二次 create 的 `Initialize` 重跑 post_init。SPDK env 是**进程级**资源（dlopen RTLD_NODELETE 常驻、mem_map_fini 无人调），与 op 实例级 `ready_` 生命周期错位。

**修复**：`DlNvmeApi::EnsureSpdkEnv()`——进程级 **once-per-success** 闩锁（dlopen + EAL + post_init 三合一；独立 envMutex 嵌套于 gMutex 之外，序固定无死锁）。失败不闩（部署修复后可不重启进程重试）。否决的替代：destroy 时调 `spdk_env_fini()` 撤回调——多池共享 SPDK 底座时会拆共享状态。
