# NVMe-oF 远端 NVMe SSD 池设计文档

> 状态：**统一传输层架构（hybm → data op → transport）定稿，P1+P2 已实施（待集群验证）**。B0-B3 批次（特性+联调+性能优化+零拷贝）已实施并集群验证；传输层收编（NvmeTransportManager 单例 + compose 第三子 transport + op 瘦身为编排层）代码已落地，UT/集群回归通过后本注记移除。演进史与踩坑档案见附录 A/B。

## 1. 背景与目标

池管理介质在 HOST DRAM / NPU HBM 之外新增第三介质 **NVMe SSD**（经 NVMe-oF，SPDK 双端直连形态），补齐冷层容量：

- **易失语义**：与 DRAM 池一致——池回收/客户端退出即数据作废，无 checkpoint、无跨池身份；
- **容量与带宽目标**：单 FAR 节点多盘聚合，数十 TB、端到端 20GB/s+；
- **写语义**：NVMe CQE = 已落盘，库层不加 flush API 面；
- **分层目标**：与 host_rdma/sdma/dev_rdma 一致的三层架构（hybm → data op → transport），传输资源进程级共享。

## 2. 用户形态与地址模型

### 2.1 统一 API（GVA 编址不变）

用户接口与现有 DRAM/HBM 池完全一致，SPDK 交互全部内化：

```c
ralloc.create(..., dataOpType |= NVME_TCP|NVME_RDMA, maxNvmeSize=...)  /* 能力与窗口声明 */
extend_remote_mem(h, MEM_TYPE_NVME, 64TB, &info)      /* info.gva 照常返回 */
smem_ralloc_copy(h, src, nvmeGva + off, len, flags)   /* 写 → 落 SSD */
smem_ralloc_copy(h, nvmeGva + off, dst, len, flags)   /* 读 → 出 SSD */
smem_ralloc_copy_batch(...)                           /* NVMe/内存段混合, 自动分流 */
pool.register(buf, size) / pool.unregister(buf)       /* 用户内存预注册（零拷贝, §6） */
```

- 用户按**字节粒度任意编址**，512B 扇区对齐在库内消化（未对齐头尾 RMW bounce）；
- NVMe 块是组内"第一公民"，全员可见可访问；
- **AICore 边界**：`device_copy/v2/v3` 对 NVMe GVA 一律 `SM_NOT_SUPPORTED`（SSD 无 MR，物理不可达）。冷层工作流 = `copy(nvmeGva→dramGva)` 预取 + 现有 v3 gather；
- NVMe 协议位 `NVME_TCP(1<<7)/NVME_RDMA(1<<8)` 互斥，且与 `DEVICE_SCHEDULE` 互斥（host 发起专用）；协议唯一真源是 op 位，FAR 在段预留时校验 target 实际监听 transport，不一致阻断。

### 2.2 GVA 分配与映射

```
分配(两级, 复用现有机制):
① 池创建: AllocReserveGva(rank, maxNvmeSize, localSize=0, NVMe)   ← 纯 VA 预留, 永不物化
   maxNvmeSize 进池定义一致性比对（同 maxDram/maxHbm, 组布局必须一致）
② extend: FAR 在属主 rank 的 NVMe 带内取 [gvaBase, gvaBase+size)（2M 对齐管理粒度）

映射(两层, 一次查表 + 一次算术):
第一层: allocatedMap_[HVM_GVA] → memType=NVMe → ClassifyAddress=GLOBAL_NVME
        → COPY_DIRECTION_TABLE(5×5) → InferCopyDirection 自动吐出方向码 → compose 分发
第二层: extent 线性仿射  startLba = lbaBase + (off >> 9), numLbas = len >> 9
```

两表分工：VaManager 只答"这段 GVA 是 NVMe 媒质、归哪 rank"；extent 明细（nqn/nsid/lbaBase/traddr）在**进程级 `HybmNvmeExtentRegistry`**，由 NVMe 段 Import 供给。

**地址模型注记（TransformVa 空实现的正确性）**：用户内存为原生 HVA/DVA（不进 GVA 体系）；NVMe GVA 恒为远端盘介质（本地 VA manager 恒无映射）→ 尽力转换恒为空操作，空实现正确（对比 host_rdma：其 GLOBAL_HOST 侧可能映射本 rank 池块，需要转换）。

## 3. 总体架构

```
                    配置面(本地 JSON-RPC, 零数据)         控制面(复用 ralloc RPC)
 NEAR 节点(每卡一进程)                                        FAR 节点
┌──────────────────────────┐                          ┌───────────────────────────┐
│ torch 进程                │  PLACEMENT ──► master    │ memfabric daemon(元数据)   │
│  smem 层(控制薄壳)         │ ◄─ JOIN_ALLOC ─► executor│  └─JSON-RPC─┐             │
│  hybm 层(entity/mm)       │     └ GroupUpdate 广播──►│             ▼             │
│   HostDataOpNvme(编排)    │        (全员导入,屏障回包) │  nvmf_tgt(独立进程)      │
│    └ NvmeTransportManager│                            │   tgt NIC ◄─ bdev(raid0)  │
│      (dlopen libspdk)    ╪══ NVMe-oF over RDMA ═════╪═══          └─ 本地 NVMe×N │
│  pinned 池 + SDMA 腿      │        host NIC ◄─────────┘                           │
└──────────────────────────┘                          └───────────────────────────┘
```

- **网卡**：双端 host PCIe NIC + rdma-core/ibverbs 用户态栈（SPDK 不支持 libra 私有栈）；host NIC 与 device NIC 分卡，存储面与 DEVICE_RDMA 数据面硬件互不抢带宽；
- **daemon 不碰数据**：数据面单网络跳，tgt CPU 零接触数据；tgt 视角 = 合法多 initiator；
- **SDMA 仅在 NEAR 内部最后一跳**：host NIC 与 NPU HBM 分属两个 DMA 域，只能 SDMA 跨 PCIe 落 HBM。

### 3.1 entity/mm 层（HybmNvmeSegment，第四种 MemSegment）

| MemSegment 虚方法 | 内存段（DRAM/HBM） | NVMe 段 |
|---|---|---|
| `ReserveMemorySpace` | VMM/大页预留 | VaManager NVMe 带预留（`AllocReserveGva`，永不物化） |
| `AllocLocalMemory` | mmap/显存分配 | **LBA 位图切区间 + JSON-RPC `nvmf_subsystem_add_ns` + `add_host` 白名单**（FAR 的 SPDK 触点）；触发 warmup 回调，失败自包含回滚 |
| `Export(slice, exInfo)` | 序列化 GVA/键 | 序列化 `{gva, lba, nqn, traddr, trsvcid, nsid}`（string 载体，无 ABI 问题） |
| `Import(allExInfo)` | 建映射/注册 | 解析 extent → `AddVaInfoFromExternal(onlyGva)` + 登记进程级 extent registry；触发 warmup（连接+预建 qpair，硬失败回滚） |
| `Mmap/Unmap` | 建解本地映射 | **no-op**（连接在导入时 warmup，见 §4） |
| `ReleaseSliceMemory` | 释放内存 | JSON-RPC `remove_ns` + 位图回收 |
| `RegisterMemory` | 用户 MR 注册 | 不支持（NOT_SUPPORTED；纯 NVMe 池的用户注册在 entity 层直接落 transport MR + 哨兵 slice，见 §6.2） |

### 3.2 data op 层（HostDataOpNvme，纯编排）

- 职责：方向判定（4 个 NVMe 方向）、GVA→extent 解析、batch 分组（per-endpoint 聚合、组失败不阻塞）、warmup 回调挂钩（registry→op 钩子，内部转发 transport connect）；
- compose 层 `GetPrioritedDataOperators` 对等加档（能力协商走实体 tag 交换）；
- Initialize：`LoadExtendLibrary(DL_EXT_LIB_NVME)` 硬失败前置（A8a）。

### 3.3 transport 层（NvmeTransportManager，进程级单例）★

`NvmeTransportManager : public TransportManager`，作为 `ComposeTransportManager` 的第三子 transport（与 device/host 平行，`GetInstance()` 进程单例——对齐 `HcomTransportManager` 先例）。承载全部传输域资源：

| 资源 | 说明 |
|---|---|
| EAL/SPDK 生命周期 | `OpenDevice` = EnsureSpdkEnv（dlopen 全家 + EAL `--in-memory --no-pci` + post_init，进程级 once-per-success 闩锁） |
| ctrlr 连接表 | key=`nqn|traddr|trsvcid`，**引用计数，跨池共享**（多 entity 复用同一 target 连接） |
| qpair 槽池 | per-ctrlr，try-lock 轮转租赁 = 并发窗口上限（无全局 IO 锁） |
| staging 双懒池 | chunk 块 / 512B 扇区两类，`spdk_dma_malloc` 域 |
| MR 表（用户内存注册） | zc 注册（§6），与 `HcomTransportManager::mrs_`、`RdmaTransportManager::registerMRS_` 同构 |
| LBA 协议适配 | BuildCmds/RMW/MDTS 拆分/窗口引擎（§5） |

**基类接口语义映射**（rank 语义经 `extent.ownerRank → target` 贯穿，全部可实现）：

| 基类接口 | NVMe 实现 |
|---|---|
| `OpenDevice/CloseDevice` | SPDK env 建立/释放 |
| `Prepare/Connect/UpdateRankOptions` | 连 ownerRank 的 target（warmup 语义；加 rank=加 target） |
| `RemoveRanks` | 断 ownerRank 的 ctrlr（引用计数归零 detach） |
| `RegisterMemoryRegion/Unregister` | 用户内存注册/注销（§6） |
| `QueryHasRegistered` | zc 命中查询（op 命令级判定的数据源） |
| `QueryMemoryKey/UpdateMemoryKey` | lkey 语义 / no-op（rkey 不适用——initiator 无被动服务） |
| `Read/WriteRemote(rankId, lAddr, rAddr=GVA, size)` | GVA→extent（校验 ownerRank）→命令展开→窗口→staging/zc→同步收割 |
| `Read/WriteRemoteBatchAsync` + `Synchronize` | 提交不收割 / 收割窗口 |

**共享收益**：ctrlr 连接与 EAL 跨池复用（连接数不随池数增长）；qpair/staging 按 ctrlr 共享；MR 表经 compose `mrs_` 统一路由。混合池（host_rdma+nvme）OpenDevice 顺序：host 先（hcom 服务）、nvme 后（EAL --no-pci 与 hcom 无冲突）。

### 3.4 smem/ralloc 层（纯控制薄壳）

- executor `OnJoinAlloc`：现有 extend 骨架原样（`ExtendLocalMem(NVME, size)` 走 ①~④），仅校验放开 + create 分支 `options.nvmeVASpace`；
- master：候选表第三列 `nvmeCommittedBytes` 簿记（零 SPDK 交互），负载 = 已提交字节 + 在途 grant；
- 混合批次分流复用 smem/bm 层既有逐段分类点；
- `RegisterMem` 的 NVMe 池分支（纯 NVMe 池走 zc 注册、混合池 MR+zc 叠加，见 §6）。

## 4. 连接生命周期与 import/export

### 4.1 extend 全流程

```
① placement → JOIN_ALLOC → FAR executor（现有路径）
② FAR: HybmNvmeSegment::AllocLocalMemory = LBA 位图切区间 + JSON-RPC 签发
   {nqn, nsid, lbaBase, lbaLen} + 白名单 add_host（hostnqn 按 rank 派生:
   nqn.2016-06.io.spdk:memfabric-pool<id>-rank<r>）
   → 实体 GroupUpdate 广播 extent（Export 格式）→ 屏障: 全员导入完成才回包
③ 各成员: NVMe 段 Import → AddVaInfoFromExternal(onlyGva=true) + extent registry 登记
   → warmup 回调: connect ctrlr + 预建 qpair slots（硬失败, 见 4.2）
④ extend 返回 = 任何成员立即可用该 gva（不变式与 DRAM 块完全一致）
```

白名单随组事件维护：join → `add_host`，rank-down → `remove_host`。一致性等级与内存块相同；NVMe 写以扇区为原子单位，多写者冲突窗口比 RDMA 大——README 边界注明，应用层版本/分区兜底。

### 4.2 生命周期前置（fail-fast）

| 阶段 | 动作 | 失败语义 |
|---|---|---|
| 池 create | `LoadExtendLibrary(DL_EXT_LIB_NVME)` dlopen 全家 + EAL init（transport OpenDevice） | **硬失败**（create 即报"SPDK 环境不可用"） |
| extent 导入/分配 | registry 触发 warmup → transport connect + 预建 qpair slots | **硬失败**：Import/AllocLocalMemory 报错并自包含回滚（va info/registry/imports_ 全撤） |
| 首 IO | endpoint 必须已存在（未命中报 `BM_NOT_CONNECTED`，不惰性连接兜底） | 破损装配显式暴露 |

首 IO 尾延迟 ~1.3s → 接近 0；部署错误（缺库/断网/协议不符）在 create/JOIN 即暴露。warmup 幂等（重复注册同 extent 不重连）。

### 4.3 故障与一致性

- 池回收/客户端退出 → `remove_ns` → extent 回收；**ns 销毁即断链**（stale qpair 读失败），陈旧 extent 条目安全退化；
- FAR tgt 不健康 → admin RPC 超时 → 容量上报清零 → 后续 extend 自动重放置；
- NEAR qpair 断链/ns 消失 → SM_* → group event → 上层重放置；
- NVMe 写完成语义 = NVMe CQE（SPDK 保证），库不加 flush 层。

## 5. 数据路径

### 5.1 端到端路径与预算

```
读: SSD ─PCIe DMA(gen4 ~7GB/s/盘,多盘聚合)→ FAR tgt buffer
    ─RDMA(~23GB/s)→ NEAR NIC ─DMA→ NEAR pinned DRAM（或 zc: 直接 user buffer）
    ─SDMA(~20-25,挂流/双缓冲)→ NPU HBM
写: 镜像反向; NVMe CQE = 已落盘
端到端(大块流式) ≈ min(NVMe 聚合, RDMA, SDMA) ≈ 18-20GB/s（多盘聚合目标）
小 IO ~百 us 级; 子 4K/未对齐: RMW bounce
```

### 5.2 命令展开（BuildCmds，纯函数）

一次请求（或整个 batch）展开为命令序列：

- **mid 段**整扇区直传，单命令 cap = min(控制器 MDTS, staging chunk)（SPDK 不代拆，nvmf TCP 默认 128KB；超过则拆多条，LBA 连续数据无缝）；
- **头/尾亚扇区** RMW：round1 读半段（读整扇区）→ merge（用户字节并入）→ round2 写整扇区；头字节 clamp 到 len（单扇区内写不越界）、尾仅当跨出首扇区才存在；
- **命令级零拷贝判定**：每条 mid 段独立判 `[userPtr, userPtr+n)` 是否完整落在注册窗内（§6）——4K 对齐 buffer 的窗外边缘段自然回退 staging，窗内段直接 DMA。

### 5.3 窗口引擎（SubmitWindowed，两轮流水）

- **窗口**：填窗（staging 懒获取 + stage-in + 提交）至 `MF_NVME_QDEPTH`（默认 16）→ poll 收割（stage-out/merge/还池）→ 补发，流水重叠取代逐命令等待；
- **错误路径**：停止补发 + 收割在飞 + 报首错；qpair 死亡（process_completions 负值）则弃单防死等；
- **batch 聚合**（对齐 host_rdma 批量模式）：全批次命令共享窗口（4K 小 IO 并发主通道），多 endpoint 按组各占窗口，组失败不阻塞其余组；
- **cpl 状态检查**：按 v25.05 nvme_spec.h 自声明前缀（16bit status word，`raw & 0x0FFE` 判 SC/SCT）置错；
- **staging**：两级懒池（chunk 块 / 512B 扇区块），活块数 ≤ qdepth；`spdk_dma_malloc` 的 NUMA 落点跟随 EAL main lcore（见 §8.3）。

### 5.4 并发模型（qpair 租赁，无全局 IO 锁）

**qpair 即并发边界**（SPDK qpair 非线程安全）：窗口全程独占一个 slot（`try_lock` 轮转，全忙阻塞 = 天然背压），并发上限 = `MF_NVME_QPAIRS`（默认 4）；单线程退化为纯轮转零开销。窗口状态全部栈/局部，slot 锁封闭并发正确性。IoCtx 非原子（SPDK 回调在 poll 线程内联执行，无跨线程竞争）。

## 6. 用户内存注册（零拷贝，MR 域）

### 6.1 机制

`spdk_mem_register(va, len)`：把 2MB 对齐 host 区间注册进 `g_mem_reg_map` → 通知 rdma mem map 整段一次 `ibv_reg_mr` → **NVMe 命令的 DMA buffer 直接指向用户内存**（读：NIC 直接 DMA 写 user buffer，省 stage-out；写：NIC 直接读，省 stage-in——消除单线程 memcpy ~4.4GB/s 天花板）。

**不适用（自动回退 staging）**：RMW 头/尾亚扇区命令（读改写整扇区语义）；device（DVA）端点（aclrtMemcpy 语义）；未命中注册窗的命令段。

### 6.2 注册策略（预注册模型）

| 机制 | 说明 |
|---|---|
| **注册通道** | 复用 ralloc `pool.register(addr, size)` / `unregister`——host_rdma 池注册 transport MR、NVMe 池注册零拷贝 MR，上层心智一致 |
| **链路（统一注册链）** | `SmemRallocEntry::RegisterMem`（统一，无 NVMe 分支）→ `hybm_register_local_memory` → entity `RegisterLocalMemory`：纯 NVMe 池（无本地段）→ `transportManager_->RegisterMemoryRegion`（compose→nvme 子 transport MR）→ 返回**打包哨兵 slice**（`MemSlice::MakeZcSentinelId`：memType=NVME + 哨兵 magic + 账本 index），entity `zcRegs_` 记 index→(addr,size)；注销统一 `hybm_free_local_memory`：三段反查 miss + 哨兵命中 → 账本取 addr → transport MR 释放。哨兵不在任何段表（GetMemSlice 必 miss），Export/GetSliceVa 等经典消费点天然拒绝 |
| **混合池叠加** | compose::RegisterMemoryRegion 尾部附加 nvme MR 尝试——失败仅 WARN 降级（零拷贝是优化，不阻断 host 主路径）；nvme-only 池该调用即注册本体，失败即失败 |
| **IO 路径** | **只查表**（`QueryHasRegistered`，ns 级），命令级命中（§5.2）；命中 → `buf=userPtr` 跳过 AcquireBuf/CopyUserIn/CopyUserOut；**IO 路径零注册开销，无阈值判断** |
| **生命周期** | 显式 register/unregister 对称；buffer 须存活至 unregister（与 host_rdma MR 同等契约）；transport teardown 清残留注册 |
| **全局开关** | `MF_NVME_ZEROCOPY`（默认 on）；`spdk_mem_register` 为 **optional** dlsym 符号，缺失则全局自动回退 staging，正确性不依赖零拷贝 |

**否决项存档**：IO 路径现注册现注销（重复负载反复 pin/unpin，纯开销）、LRU 自动缓存（VA 复用 → DMA 数据损坏的结构性风险）。

### 6.3 SPDK 侧约束（v25.05 源码实证）

`spdk_mem_register` 要求 vaddr/len **均 2MB 对齐**（否则 EINVAL）、不可重叠（EBUSY）、unregister 须同区间头尾配对（ERANGE）——内窗收缩 + 按句柄注销天然满足；notify 回调**整段一次**（60MB 内窗 = 一次 `ibv_reg_mr`）；**不要求大页**（RDMA 数据面用 ibv_reg_mr pin 普通 4K 页；vtophys/PRP 的大页要求只属本地 PCIe NVMe 路径）；注册计入 **RLIMIT_MEMLOCK**（例 16 两内窗 ≈120MB > 默认 64MB，需 `ulimit -l unlimited`）。已知坑（已加固）：注册失败时 SPDK 先置页标记后跑回调且**不回滚**——残留标记使同区间重试 -EBUSY；transport 在注册失败后立即补一次 unregister 清标记（对无 MR 页是安全 no-op）。

## 7. SPDK 运行边界

### 7.1 交互触点

| 端 | 交互形态 | 代码层 | 载体 |
|---|---|---|---|
| NEAR/成员侧（数据面） | 进程内库调用（dlopen libspdk，`spdk_nvme_*` C API） | transport 层 | NvmeTransportManager + `dl_nvme_api` |
| FAR 资源端（控制面） | 进程间 JSON-RPC（本地 unix socket → 独立 nvmf_tgt） | hybm 层 | HybmNvmeSegment 内嵌 admin client |
| master 侧 | 零 SPDK 交互 | smem 层 | 候选表第三列 |

FAR 端 RPC 方法 × 生命周期：daemon 启动 `--nvme`（`framework_get_rpc_methods`/`bdev_get_bdevs` 探活+容量）→ 部署脚本一次性（`bdev_nvme_attach_controller`/`bdev_raid_create`/`nvmf_create_transport`/`nvmf_create_subsystem`/`add_listener`）→ JOIN_ALLOC 签发（`nvmf_subsystem_add_ns`，嵌套 namespace 子对象，响应为裸数字 nsid）→ 成员 join/rank-down（`add_host`/`remove_host`）→ 池回收（`remove_ns`，平铺 `{nqn,nsid}`）。tgt 探活失败 → RPC 超时 → 容量清零 → 自动停止放置。

SPDK 的 MR 只覆盖 NEAR host DRAM（staging + zc 注册区），永远看不到 HBM/GVA——两域间只有库调度 SDMA。

### 7.2 dlopen 宿主依赖链（SPDK 25.05 实证）

SPDK 共享库（`--with-shared`）部分运行时依赖**不进 DT_NEEDED**，dlopen 宿主必须以 `RTLD_NOW|RTLD_GLOBAL` 预载注入全局符号表（`dl_nvme_api.cpp` 的 `PreloadDeps` 内置）：

| 环 | 符号 | 库 | 要点 |
|---|---|---|---|
| 1 | `EVP_MD_CTX_free` | libcrypto | 系统包 |
| 2 | `crc16_t10dif` | libisal | SPDK 默认静态链入，需 `<spdk>/isa-l` 手动产共享版部署到 `build/lib` |
| 3 | `rte_mempool_ops_table` 等 | DPDK 全家 | 目录扫描预载：`MF_NVME_DPDK_LIB_DIR` 显式优先，默认遍历 `LD_LIBRARY_PATH`（须含 spdk 与 dpdk 双 build/lib） |
| 4 | `OPENSSL_init_ssl` | libssl | 系统包 |
| 5 | sock 实现 | `libspdk_sock_posix.so` | 经 constructor 注册进框架并设默认；dlopen 宿主不预载则 `spdk_sock_connect_ext` 对空注册表**静默返回 NULL**（strace 无 connect）。顺序约束：sock 实现必须在 env 库之后加载（`libspdk_util` 引用 env 层 `spdk_realloc`） |

### 7.3 EAL 初始化（external-EAL 动态注册模式）

`InitDpdkEnv`：`rte_eal_init("memfabric-nvme","--in-memory","--no-pci")` ——免共享配置文件；`--in-memory` 不免大页本身（按需动态取用，进程退出即还，系统 2M 池须有余量——tgt 部署脚本已抬至 4GB）；**`--no-pci` 必需**（与同机 tgt 的 PCI 探测冲突，附录 B 坑一）；高 NUMA node（物理核 ≥ RTE_MAX_LCORE=128）自动注入 `--lcores=0@<first>`（坑三）；eal_init 负返回区分 `EALREADY`（良性）与真失败。进程级 `EnsureSpdkEnv` once-per-success 闩锁（dlopen+EAL+post_init 三合一；post_init 不可重入——坑二）。

### 7.4 Keep-Alive 必须禁用 + opts 契约

SPDK 默认 KAT 10s 需宿主周期轮询 admin qpair——与按需轮询设计冲突，connect 时显式 `keep_alive_timeout_ms=0`。**opts_size 契约**：必须传完整 856B 不透明 buffer（`get_default_ctrlr_opts(buf, 856)` 全量默认后按 20B 前缀偏移写 KAT=0）；传部分前缀会使 opts 尾部指针域（tls_psk/dhchap_key）保持栈垃圾 → keyring 解引用 SIGSEGV。

### 7.5 initiator ABI 要点（gdb 栈实证）

| 坑 | 对策（已内置） |
|---|---|
| `spdk_nvme_connect` 自 24.09 起三参 `(trid, opts, opts_size)` | typedef 三参；官方 NULL/0 路径（零 TLS） |
| `spdk_nvme_transport_id` 布局 sizeof=596（char 数组后无对齐垫） | 按上游字段序自声明 + `static_assert(sizeof==596)`；trtype 显式填（rdma=1/tcp=3）、adrfam=IPv4(1)（0=INVALID 被拒） |
| `spdk_env_dpdk_post_init(bool)` 单参 | typedef 对齐 |

## 8. 资源管理与部署

### 8.1 多进程（一节点 8 进程）

| 项 | 对策 |
|---|---|
| CPU | 按需轮询无常驻线程；纯 DRAM/HBM 池进程零成本（懒加载+硬失败前置） |
| 内存 | staging 受控（`MF_NVME_BUF_MB`×`MF_NVME_BUF_SLOTS`，动态大页取用）；zc 注册计入 RLIMIT_MEMLOCK |
| 连接 | 8 进程 = 8 独立 initiator（NVMe-oF 天然多主机）；tgt 限额内 |
| 冲突面 | uverbs 多进程安全；EAL 每进程独立（--no-pci 后与 tgt 同机无冲突）；争抢点仅 hugepage 池/memlock/轮询核 |

### 8.2 hugepage 口径

tgt 侧系统池 2048×2M=4GB（脚本抬）+ tgt 进程 `-s 2048`；NEAR 侧 `--in-memory` 按需动态取用 ≈ `QDEPTH × BUF_MB + ~100MB`（默认 ~134MB，建议预留 1GB）；`nr_hugepages` 全局共用，2M 页足够。

### 8.3 NUMA 亲和

数据 NIC 与其所在 NUMA node 对齐（本集群 NIC=node6, cpus 144-167；NPU c1/c2 亦在 node6，device 方向存在全亲和组合 `--dev 6/7`）：

- **NEAR**：`client.py --numa-node N` 一次绑核三对齐（copy 线程/staging first-touch/qpair poll）；
- **FAR**：setup 脚本按 listener IP 反查网卡 NUMA，tgt reactor 经 `--lcores 0@<cpu>,1@<cpu>` 绑核；
- **staging NUMA 落点**：`spdk_dma_malloc` 透传 SOCKET_ID_ANY → DPDK 按 EAL init 线程的 `rte_socket_id()` 落点——`--lcores` 映射顺带绑定 staging 到目标 node，无需显式 socket 分配；
- 亲和收益实测：read 1M +20%、16M read +8%；write 侧被流水掩盖零差异（天花板在 memcpy 带宽本身，靠零拷贝消除）。

## 9. 环境变量

| 变量 | 侧 | 默认 | 说明 |
|---|---|---|---|
| `MF_NVME_QDEPTH` | NEAR | 16（clamp 1..128） | 窗口深度（单窗在飞命令数） |
| `MF_NVME_QPAIRS` | NEAR | 4 | qpair 槽数 = 并发窗口上限 |
| `MF_NVME_BUF_MB` | NEAR | 4 | staging 单段上限（段=min(MDTS, BUF_MB, 尺寸)） |
| `MF_NVME_BUF_SLOTS` | NEAR | =QDEPTH | staging 池深（两类池各计） |
| `MF_NVME_ZEROCOPY` | NEAR | 1 | 零拷贝总开关（0=全量回退 staging） |
| `MF_NVME_DPDK_LIB_DIR` | NEAR | — | DPDK 库目录显式指定（默认遍历 LD_LIBRARY_PATH） |
| `MF_NVME_EXTENT_GRAN` | FAR | 1G | extent LBA 粒度（须 ≤ 盘容量） |
| `MF_NVME_NQN`/`TRADDR`/`TRSVCID` | FAR | — | target 对外身份 |
| `MF_NVME_RPC_SOCK` | FAR | /var/tmp/spdk.sock | JSON-RPC socket |
| `MF_NVME_BASE_BDEV` | FAR | — | 底座 bdev（真盘 Nvme0n1 / 模拟 Malloc0） |

应急回退位：`QDEPTH=1 + QPAIRS=1` 近似串行行为。协议位无 env（唯一真源=op 位）。

## 10. ABI / 接口清单（append-only，旧二进制互操作安全）

| 触点 | 变更 |
|---|---|
| `smem_ralloc_def.h` | `SMEM_RALLOC_MEM_TYPE_NVME`、`SMEMRA_DATA_OP_NVME`、mem_info 追加 nvmf 三元组 |
| hybm 公共头 | `HYBM_DOP_TYPE_NVME_TCP/NVME_RDMA`、`HYBM_MEM_TYPE_NVME`、`HYBM_MST_NVME`、`maxNvmeSize`/`nvmeVASpace`、方向枚举 4 个（13-16） |
| `hybm_va_manager` | `GLOBAL_NVME` AddrType、`COPY_DIRECTION_TABLE` 5×5 |
| C API | `hybm_query_nvme_extent`；用户内存注册复用 `hybm_register_local_memory/free_local_memory`（纯 NVMe 池返回打包哨兵 slice，无新增符号） |
| RPC msg | v3（160B，`maxNvmeSize`/`nvmeCommittedBytes`） |
| 校验面 | NVMe 媒质/地址进 `device_copy/v2/v3` → `SM_NOT_SUPPORTED` |

## 11. 观测（TP 打点 9 个）

`TP_HYBM_NVME_COPY_TOTAL / LOOKUP / STAGE_COPY / SUBMIT / POLL_WAIT / RMW_STAGE_B / QPAIR_WAIT / PRELOAD / ZC_REG`（ptracer 机制）。`QPAIR_WAIT` 观测多线程背压；`POLL_WAIT/STAGE_COPY` 定位拷贝与等待占比；`ZC_REG` 注册耗时；零拷贝命中率由 `STAGE_COPY` 下降量观测。

## 12. 验收标准与性能基线

### 12.1 特性验收（均已通过）

| 项 | 结果 |
|---|---|
| UT | 特性批 23/23 → B2 批 18/18 → B3 批 24/24（窗口/RMW/qpair 租赁/warmup 回滚/零拷贝命中与回退） |
| sim 双形态 | TCP loopback（正确性）+ 跨机 RDMA 模拟盘全流程（round-trip/RMW/MDTS 拆分 BW/干净退出） |
| device 方向 | LOCAL_DEVICE↔GLOBAL_NVME 四方向（torch_npu DVA + AUTO 推断；torch H2D 异步流须 `npu.synchronize()`） |
| 稳定性 | FAR daemon 不重启，NEAR 多轮 create/destroy/BW；warmup 幂等 |
| 性能 | 见 12.2 演进表 |

### 12.2 性能演进（33↔35 RDMA，write/read GB/s，各阶段实测）

| 尺寸 | 4K | 64K | 1M | 16M |
|---|---|---|---|---|
| 基线（深度=1 串行，2026-10-22） | 0.37/0.37 | 2.32/2.74 | 2.69/3.37 | 1.81/2.62 |
| B2 窗口引擎（10-23） | 0.31/0.30 | 2.16/2.51 | **5.97**/4.57 | **6.71**/5.54 |
| +NUMA 亲和（10-23） | — | — | ~5.9/**4.85** | ~6.3/**6.91** |
| B3 零拷贝（目标） | — | ~4.3/4.5(已注册) | **≥6.7（2× 达标）** | 逼近网络/tgt 上限 |

机理：B2 前 4K/64K 单命令无窗口收益（符合预期）；read 1M 曾卡 stage-out 串行（STAGE_COPY 占 COPY_TOTAL 35-50%，单线程 memcpy 4.4GB/s）——B3 直接 DMA 消除。16M 后续若停在 ~7GB/s 则上限移交 tgt/RDMA 线路（另行优化）。

## 13. 测试与文件清单

| 文件 | 说明 |
|---|---|
| `src/hybm/csrc/mm/hybm_nvme_segment.*` | NVMe 段（签发/Export/Import/回滚）+ extent registry（warmup 回调） |
| `src/hybm/csrc/data_operation/host/hybm_data_op_nvme.*` | 数据面 op（编排层） |
| `src/hybm/csrc/transport/nvme/nvme_transport_manager.*` | NVMe 传输域（单例共享：EAL/ctrlr 连接表/qpair 池/staging/MR 表/MDTS+RMW+窗口引擎） |
| `src/hybm/csrc/under_api/dl_nvme_api.*` | dlopen libspdk + PreloadDeps + EAL + optional 符号 |
| `src/hybm/csrc/transport/compose/compose_transport_manager.*` | 第三子 transport 路由（收编后） |
| `test/ut/testcase/hybm/mm/hybm_nvme_segment_test.cpp` | 段 UT（AIO/malloc bdev，无需真盘） |
| `test/ut/testcase/hybm/data_operator/hybm_data_op_nvme_test.cpp` | NVMe 数据面 UT 24 用例（经 op 公有接口黑盒测试，NvmeApiStub 手写桩钉在 DlNvmeApi 层不随层动；私有断言经单例捅 transport；SetUp/TearDown ResetInstanceForTest 隔离共享态） |
| `examples/memory_pool/02_scale_out/16_nvme_ssd_pool/` | client.py（--device/--numa-node/--no-zc）+ setup_nvmf_tgt.sh（RDMA/NUMA）+ README |

## 14. 风险与回退

| 风险 | 回退 |
|---|---|
| host NIC 无 rdma-core provider | 内核栈变体（nvmet/内核 initiator），API 面不变 |
| DPDK 侵入 torch / 动态注册损耗 | 每节点 sidecar initiator |
| 存储面/数据面共卡争抢 | trsvcid 4420 + VLAN 隔离；硬件本就分卡 |
| 旧部署无 SPDK | dlopen 可选加载，优雅 NOT_SUPPORTED；构建不硬链接 |
| memlock 不足（zc 注册失败） | `ulimit -l unlimited`；未注册自动回退 staging |
| 多写者冲突窗口 | 应用层版本/分区（README 边界） |
| 零拷贝 VA 复用 | 显式 register/unregister 契约（与 host_rdma 同级）；无缓存模式结构规避 |

---

## 附录 A. 实施演进年表

| 批次 | 日期 | 内容 | 关键成果/偏差 |
|---|---|---|---|
| B0 甄别 | 2026-10 | host NIC verbs 可用性/RDMA 带宽/动态注册量化/8 进程并发 | P0 全过，SPDK TCP initiator 可用 |
| B1 特性批 1 | 10-15~17 | ABI 骨架 + HybmNvmeSegment，UT 4/4 | 定稿基线 |
| B2 特性批 2-3 | 10-18~20 | dl_nvme_api + HostDataOpNvme + compose/pymf + example 16；UT 23/23 | sim 双形态全通（TCP loopback 0.03 / RDMA 3.37GB/s）；管理面签发链修 8 个协议适配 bug（split_size_mb 单位、split 命名 `<base>p<i>` 从 0 起、add_ns 嵌套 namespace 子对象、raid JSON 键 `level`、EPIPE 只容忍 exist 类错误、GVA 带泄漏、JOIN_ALLOC 失败补 UnInitialize、失败自包含回滚收敛到 SmemRallocEntry::Initialize）；RPC 报文 v3 仅追加两控制面字段；NVMe 位显式化为 NVME_TCP/NVME_RDMA 互斥位；协议唯一真源=op 位（MF_NVME_TRSTRING 删除） |
| B2 性能优化 | 10-23 | 窗口引擎+qpair 租赁+生命周期前置+TP+UT 18 用例 | write 1M 2.2×/16M 3.7×、read 16M 2.1×；联调修 BuildCmds 头尾越界、post_init 重入（坑二）、--no-pci（坑一） |
| device 方向 | 10-23 | LOCAL_DEVICE↔GLOBAL_NVME 四方向 | torch_npu DVA 端点；修 torch H2D 异步流跨流同步 |
| NUMA 亲和 | 10-23 | L1 client 绑核 + L2 tgt --lcores | read 1M +20%/16M +8%；坑三（高核 lcore 映射）；staging NUMA 源码链实证（L3 无需） |
| B3 零拷贝 | 10-26 | 预注册模型：optional 符号+ZEROCOPY 开关+命令级命中+分发链+UT 24 | SPDK mem_register 约束源码实证（2MB/整段 reg_mr/无大页要求/memlock/失败残留清理） |
| 传输层收编 | 进行中 | NvmeTransportManager 统一三层+进程级共享 | 评审修正两项：NVMe 有 rank 语义（ownerRank→target 贯穿）；共享模式对齐 host 子 transport GetInstance 先例。zc 链归一：`_zc_` C API/虚接口转发链删除，注册复用 `hybm_register_local_memory` + 打包哨兵 slice（entity 账本路由注销），compose MR 统一路由（混合池 addon 降级语义） |

## 附录 B. 踩坑档案（防再犯，均源码/ gdb 栈/日志实证）

### B.1 EAL PCI 探测冲突（tgt + daemon 同机）

FAR daemon EAL 无 `--no-pci` → 与常驻 tgt 撞 PCI → eal_init 真失败被当 EALREADY 容忍 → post_init 在半死 EAL 上失败又被 WARN 吞 → `g_mem_reg_map=NULL` → 首 connect 在 `mem_map_notify_walk` 的 `if (!g_mem_reg_map) return -EINVAL`（SPDK 全路径**唯一无日志**失败点，现场只见 "Unable to create memory map"）。修复：argv 加 `--no-pci`（NVMe-oF 走 verbs 非 PMD，本就不需要 PCI）；eal_init 负返回区分 EALREADY 与真失败；post_init 失败硬失败化。

### B.2 `spdk_env_dpdk_post_init` 不可重入

v25.05 `mem_map_init` 每次 `rte_mem_event_callback_register("spdk",...)`，DPDK 同名回调二次注册 EEXIST → mem_map_init 失败。SPDK env 是进程级资源（RTLD_NODELETE 常驻），与 op 实例级生命周期错位——daemon 不重启的二次 create 暴露。修复：`EnsureSpdkEnv` 进程级 once-per-success 闩锁（失败不闩，可重试）。否决项：destroy 时 `spdk_env_fini()` 撤回调（多池共享时会拆共享状态）。

### B.3 高 NUMA node 的 EAL lcore 映射

进程绑到 node6（cpus 144+）后 EAL 报 `Invalid 'command line' arguments`——物理核 ≥ `RTE_MAX_LCORE`(128) 映射失败；`-m/-c` 掩码同理被拒。两侧统一 `--lcores <id>@<phys>` 映射解决（顺带正确绑定 staging NUMA）。

### B.4 `spdk_nvme_connect` opts_size 三参 ABI（24.09+）

旧单参调用把寄存器残留垃圾当 opts → 垃圾尾部的 `tls_psk/dhchap_key` 指针进 keyring 解引用 SIGSEGV。正确：三参 + `(trid, nullptr, 0)` 或完整 856B buffer（全量默认后按 20B 前缀偏移改字段；**部分前缀不可作 opts_size**）。

### B.5 KAT 断链（target 侧 keep-alive）

默认 KAT 10s 需宿主周期轮询 admin qpair；按需轮询 → KAT 从未发送 → tgt 整 10s 断链（IO 报 `CQ transport error -6`）。connect 显式 `keep_alive_timeout_ms=0`。

### B.6 BuildCmds 头尾越界（B2 联调）

head 字节未与 len 取 min（单扇区内写越界）、tail 未判同扇区 → 负偏移指针 memcpy 堆破坏。修复：`min(512-headSkip, len)` + `lastSector != firstSector` 守卫。

### B.7 zc 注册失败残留（B3）

SPDK 注册失败不回滚页标记 → 同区间重试 -EBUSY。加固：失败后立即补一次 unregister 清标记（对无 MR 页安全 no-op）。

### B.8 torch 异步流与 aclrtMemcpy 跨流

`.to("npu")` 异步 H2D 与 aclrtMemcpy 无跨流隐式序——`fill` 后必须 `torch.npu.synchronize()`。
