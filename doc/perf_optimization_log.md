# 性能优化记录（c93599ef → 8c5bc1b3）

> 范围：`c93599ef`（亲和映射，2026-09-15）至 `8c5bc1b3`（刷新README，2026-10-12），
> 约 110 个提交。按数据面分组罗列性能优化点、机制与量化收益；负结果（尝试后回退）
> 一并收录——它们定义了各路径的能力边界，均已在对应 README 判读中留档。
>
> 测试基线：FAR（10.90.22.35，daemon）+ NEAR（client dev 1），200G RoCE 单口，
> CANN 9.0.0，bisheng 安装期内核。详细矩阵见
> `examples/memory_pool/02_scale_out/14_device_scheduled_rdma_bandwidth/README.md`。

## 0. 总览：优化弧线

**设备调度面（ex14，2K 粒度，N=4，us/block）**

```
2.10 (N=1 单QP) → 0.80 (多QP) → 0.59 (SEG_MAX=120 实验) → 0.43 (DVA 整批单次提交)
    → 0.37 / 0.23 (doorbell 批量 K=32 + v3 uniform，写/读)
累计：写 5.7×，读 9.1×
```

**host 调度面（ex08，64K 批量，GB/s）**

```
写 14.3 → 17.4，读 13.3 → 18.6   (多 QP lane + 多线程逐 WQE)
同时钉死天花板：libra RaSendWrV2 内 ~1.9us 串行核 ⇒ host 进程 ~280K WQE/s
```

---

## 1. 设备调度面主线（ex14）

### 1.1 fast path 显式路由（`f0b6c2f7`）

- **机制**：`device_copy` 直传 `peer_rank`/`is_write`（单发）+ 批量五描述符数组
  DVA 暂存（`stage_batch_desc`），跳过 C 层逐段地址预检（SegCheck 的全量
  alloc-range 查询）。信任契约：路由与描述符内容由调用方自负，错误路由在内核侧
  MR 查找 miss 处安全跳过（WQE 不投递，仅回读校验可见）。
- **收益**：host 提交墙钟塌缩 **60×**（8192 段 submit W：1.94ms → 0.03ms）；
  稳态吞吐不变（本就设备 bound），收益形态是 host CPU 释放与图捕获友好。

### 1.2 多 QP lane —— 设备面（多QP测试系列 + `3c03aa9a` 正式实现）

- **机制**：`MF_QPS_PER_PEER`（钳制 [1,4]，默认 1）：每对 rank 建 N 条 QP，全部
  lane 连同一 `(ip, port)`、以 tag 区分（lane 0 = 原空 tag 主连接，lane i≥1 =
  `mf_q2_<clientRank>_<i>`，client 侧 rank 生成、双端一致——与 HCCL
  `HcclSocketManager` 生产接线同款，route-A v5 实验验证本 hccp 构建支持）。
  内核 `blockDim = N`，block b 独占 lane b（SQ head 单生产者索引，1:1 绑定免锁），
  段 j → lane `j % N` 确定性分发（graph 重放一致）；每 block 末尾只 quiet 自己
  lane 的 distinct peer，N 路 quiet 并行等待。
- **收益**：2K 粒度 2.10 → **0.80 us/block**（N=4）；1→2→4 近线性，N=8 被钳制；
  大 IO（≥128K）线速持平。
- **残余发现**：小 IO 区每 chunk 串行 quiet（~1.8us/段）成为下一靶点——由 1.4 移除。

### 1.3 负结果：doorbell 批量第一版（`87d73c2e` 尝试 → `4ef270e3` 回退）

- **机制**：一次 doorbell 覆盖整段 fill（fill 全部完成后才发布）。
- **结果**：各粒度劣化 **6-9%**——把 WQE fill 串行前置于传输，破坏内核↔NIC 流水重叠。
- **价值**：与 1.5 的"逐 K 流水发布"本质不同，失败原因留档避免重蹈。

### 1.4 DVA 整批单次提交 —— `device_copy_batch_v2`（`d54ce21c`）

- **机制**：五 SoA 描述符数组（src/dst/size 为 uint64、rank/write 为 uint32，
  32B/段）驻留 HBM，host 只传 7 个标量；**一次 kernel launch 驱动整批**（上限
  2^20 段）——无 SEG_MAX 分段、无每 chunk 管线排空；lane 内在途 WQE 以 2048 段
  子批 quiet 保 SQ 环深（8192）安全。staging 同 stream H2D，流序保证复用无竞态。
- **收益**：0.80 → **0.43 us/block**（2K@64M）；**eager 与 graph 首次打平**
  （≥16M 档 ±2%，eager 略优——省图回放派发）；SEG64 时代的分段开销
  （launch + quiet ≈ 9.3us/chunk）整体移除。
- **成本模型（初版）**：`t ≈ max(0.45us, size/23.5GB/s)`。

### 1.5 SQ doorbell 批量（`a67b51f4`）

- **机制**：hns SQ doorbell 为 PI（producer index）语义——一次 doorbell 发布
  "到 head 为止的全部 WQE"（与主线 hns_roce_v2_post_sends 按次批量同款）。投递
  原语拆分为 `post_send_nowait`（WQE+SGE 构造 + 逐行 dcci，head 由调用方寄存器
  持有）与 `post_send_flush`（doorbell + head copy-pipe 回写）；攒 K 个 WQE 摇
  一次（`MF_DVA_DB_BATCH`，**默认 32 = 实测饱和点**，钳制 [1,2048]）。flush 点：
  攒满 K / peer 切换 / lane 预算 quiet 前 / 批尾（quiet 按 flushed head 轮 CQ，
  未 flush 必挂死）；K 调度是描述符数组纯函数 ⇒ graph 重放确定。
- **依据三层**：vendored 头 DB_PI 位域语义、主线 hns 驱动 post_sends 批量模式、
  K 剂量实验硬件实证（K=32/64 round-trip 全过）。
- **收益**：单 lane 每段 -0.3us（发布块 = 3×PipeBarrier + MMIO + head 回写）；
  N=4 读方向 **0.45 → 0.23 us/block（2×）**、写 0.43 → 0.37（写引擎地板主导，
  发布块在读路径占比更高故读增益更大）；K=32→64 持平。
- **边界**：`count % K != 0` 由"攒满即冲 + 尾部兜底"覆盖，无整除假设。

### 1.6 v3 uniform 批量 —— `device_copy_batch_v3`（`afb313e1`）

- **机制**：uniform 契约进 ABI——整批共享单 peer（`peerRank`）、单方向
  （`isWrite`），且 src/dst 数组各落在所属端点**同一个 mkey** 内（调用方担保）。
  仅三描述符数组（24B/段）；内核每批（每 lane）一次解析双端 MR
  （`SmemRallocUniformMr{key, delta, lo, hi}` 寄存器化），主循环零分支、WQE 寄存器
  模板化（每段仅 owner 位/长度/两个翻译地址变化）；每段 4 次比较范围自检保留
  v2 安全退化语义。loader 侧 v3 符号可选 dlsym（旧内核 .so 优雅降级）。
- **收益**：单 lane 0.96 → **0.63 us/WQE（-31%）**；与 doorbell 批量独立叠加
  （剂量链：v2 K=1 1.36 → v2 K=32 1.03 → v3 K=1 1.13 → v3 K=32 0.71/0.63 w/r）；
  N=4 下收敛到引擎天花板（v2/v3 等值），仅读方向余量档位可见净增益（4K 读 +15%）。

### 1.7 成本模型（终版，双引擎）与天花板

```
t_write(size) ≈ max(0.36us, size / 23.5GB/s)     # 写引擎串行段 0.36us
t_read(size)  ≈ max(0.22us, size / 23.5GB/s)     # 读引擎串行段 0.22us
```

- 2K-8M 全粒度 12 数据点命中（误差 ≤1%）；≥16K 双向 wire 界 21.8-22.4 GB/s
  （与历史 22.28 同口径持平，零回归）。
- 引擎天花板（N=4 聚合）：写 ~2.8M、读 ~4.3M WQE/s，**与内核路径无关**
  （v2/v3 同值）——RoCE 写/读引擎与完成路径的硬件属性；N=4 共享段构成瓶颈，
  淹没单 lane 的 uniform 收益。
- 推荐档位：`MF_QPS_PER_PEER=4` + `--batch --v3`（满足 uniform 契约时）或
  `--batch --v2`（通用路由）+ `--batch-size ≥32M`；`MF_DVA_DB_BATCH` 默认 32。

---

## 2. host 调度面（ex08）

### 2.1 负结果：wrlist 批量投递五轮尝试（`dd514b1c`→`9a7ff5f1`，回退 `bc971918`）

- **尝试**：`RaSendWrlistExt` 整批 WQE API（首版）→ QP 级 `RaMrReg` 本端+对端窗口
  注册 → pre-connect 双端注册钩子 → `enabled2mbLite` 开关 → QP mode 4→0 切换。
- **结果**：全部失败（128103 / 128102 / 528102）。根因取证：mode-4 QP 走 HDC-lite
  快路径（进程内 `local_mr[RA_MR_MAX_NUM]` 表，host 侧注册手段填不进）；mode-0 走
  verbs 路径但破坏我方单发 doorbell 流任务机制。附带发现 RaMrReg 钩子泄漏
  128MB/次（已随回退移除）。
- **价值**：钉死"闭源契约不可达"，反向论证 host 面应走 lane+多线程，批量小 IO
  终局在设备调度面。

### 2.2 host 多 QP lane + 多线程逐 WQE（`686aecef`）

- **机制**：
  - **连接面**：JoinableRanksQpManager lane 化——`ConnectionChannel` 字段数组化、
    白名单/connect 带 `mf_hl_<clientRank>_<lane>` tag（与设备面 `mf_q2_` 前缀区分）、
    tag 作用域收取、all-or-nothing（任一 lane 失败整 rank 拆除重试）；基类新增
    `GetLaneQpHandleWithRankId`/`GetQpLaneCount` 虚接口（FixedRanks/Bipartite 默认
    单 lane 零改动）。
  - **数据面**：`Write/ReadRemoteBatchAsync` 下放 cpp → 跨步分片（index i → lane
    i%lanes），lane 0 在调用线程、lane 1..N-1 在**常驻 worker 池**（避免按 tid
    缓存的 HybmStream 无限膨胀）；`Synchronize` 逐 lane notify（FENCE 语义按 QP
    独立）；块数 <256 或单 lane 走原串行路径（零回归保底）。
- **收益**：64K 批量写 14.8 → **17.4**、读 13.3 → **18.6 GB/s**（N=2，甜点位）；
  ptracer 分解证明分片生效（SUBMIT_TASK 不随并发膨胀）。
- **天花板定量**：libra `RaSendWrV2` 内 **~1.9us 串行核**（单次墙钟 2.76→3.64→
  7.63us 随线程数膨胀），模型 `R(N) = 1/(1.9us + 2.26us/N)` 三档拟合误差 <1%
  ⇒ host 进程投递上限 ~280K WQE/s；N=4 争用反噬（写 17.44→17.14），推荐 N=2。
  结构性结论：64K 逐 WQE 路 host 面可贴 wire（~20+），≤32K 离散块终局在设备面。

---

## 3. 能力铺路（性能路径的使能项）

| 能力 | 提交 | 与性能的关系 |
|---|---|---|
| 设备调度数据面落地 | `6e3dde4f` 起约 20 提交 | ex14 全部优化的载体：AICore 内核自发 WQE/自收 CQ/自 quiet、meta 窗口发布、安装期 bisheng 内核 |
| 用户注册 HBM | `c48fbc29` | 注册张量作本地端点（`USER ↔ PEER` 两腿），免本地 DRAM 槽 |
| 内存注册系列 | `7d46c893`、`f8ed058f` | 用户内存/共享内存注册进内核可见表 |
| 池内多 MR 注册 | `cf312259` 系列 | 每 rank MR_SLOTS_PER_RANK=8 槽，v3 范围自检与 uniform 解析的基础 |
| 大块分配 | `1441d1b5` | 大 IO 粒度测试前提 |
| host-DRAM 注册 | `9bbf2d0a` 系列 | HalHostRegister iova 路径 + 4K 对齐，DRAM 端点合法化 |
| 56-bit GVA | `874a56b0` 系列 | 地址空间模式 + 恒等检测/泄露加固 |
| 亲和映射 | `94dda929` 等 | conn_based_segment NUMA 亲和 |
| SGE SIZE 64 | `5caaa5cf`、`7cf7b2dd` | 内核编译参数调优 |
| host 批量接口 | `fd192115` | `smem_ralloc_copy_batch` API + python 绑定（ex08 批量入口） |

## 4. 度量与稳定性支撑

- **ptracer 体系**：`30b2b2b2`（TP trace）→ `41977a76`（休眠可控）→ `a557a93c`
  （可配置）——后续所有瓶颈分解的度量 backbone：SEND_WR/SUBMIT_TASK 逐 WQE 定价
  定位 libra 串行核、doorbell 发布块剂量定价、host 批量 ptracer 分解。
- **正确性加固**：竞态互斥、并发 join 互斥、在途入账、同步屏障、double-free/
  core dump 修复系列——保障剂量实验可复现（plog/ptracer/round-trip 三重判据）。
- **部署与用例**：集群部署脚本、端口统一、超时放宽、用例调整。

## 5. 经验沉淀（已写入对应 README 判读）

1. **负结果要留档**：doorbell 第一版（整段 fill 一次 commit）与 wrlist 五轮的失败
   机制均已写入文档，防止重试已知死路。
2. **剂量实验定价一切**：SEG_MAX 剂量（9.3us/chunk）、K 剂量（饱和点 32）、
   lane 剂量（N=4 甜点、N=8 钳制）、读写引擎天花板（2.8M/4.3M WQE/s）全部由
   控制变量实验得出，模型拟合误差 ≤1%。
3. **信任契约 + 安全退化**：显式路由/v2/v3 的错误输入一律"WQE 跳过、仅校验可见"，
   不炸硬件不挂死——高性能路径的容错底线。
4. **graph 确定性**：lane 分发、doorbell K 调度均为描述符数组纯函数，图重放与
   eager 天然一致（首次打平）。
