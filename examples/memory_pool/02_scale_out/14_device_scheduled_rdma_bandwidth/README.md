# 14_device_scheduled_rdma_bandwidth

## 场景
**设备调度 RDMA 的多粒度带宽测试**（08 的测试骨架 × 09 的数据面）：

- 池以 `DEVICE_RDMA | DEVICE_SCHEDULE` 创建（拓扑与 08/09 相同，daemon 原样复用）：
  AICore 内核自发 WQE、自收 CQ、自 quiet，单次拷贝零 host 交互；
- **本地端点全部为用户注册的 HBM 张量**（`handle.register`），不提交本地 DRAM 槽
  （无 `extend_local_mem`）：`device_copy` 走两条合法腿——张量→FAR 槽、FAR 槽→张量，
  即 09 warmup 已验证的 `USER ↔ PEER` 组合；
- 每个粒度：图外 warmup（首次 `device_copy` 会 `dlopen` 内核库，捕获中非法）→
  WRITE 计时 → READ 计时 → 回读校验，输出行与 08 同格式，结果可直接对照；
- 计时模式可开关，四种模式跑**同一槽轮转拷贝序列**（同地址、同字节数），两两对照分别量化
  图捕获、批量接口及组合的收益：
  - 默认 **direct**：同序列循环直发 `device_copy`（纯入队不同步），末次
    synchronize 计入——基线；
  - `--graph` **graph**：每方向捕获**一张完整图**——图内含整个轮转序列（blocks 份单拷贝内核，
    默认 `batch-size = real-pool-size` 时恰好全槽每个地址各写一遍），重放 `--replays` 次；
    即真实场景形态（完整图中内嵌整段槽轮转通信），重放期间 host 零参与；
  - `--batch` **batch**：同序列**一次批量直发提交**。+fast：五描述符数组经
    `stage_batch_desc` 打包 H2D 暂存 HBM 后走 `device_copy_batch_v2`——**单次 kernel
    提交**，无分段、无中途 quiet；不带 fast：checked 路径 `device_copy_batch`
    （C 层按 64 段/launch 分块、每 distinct peer 一次 quiet）；
  - `--graph --batch` **graph-batch**：捕获**一次批量提交**入图并重放——重放零 host
    参与；+fast 时图内仅含单个 DVA kernel 节点（描述符暂存在捕获前一次完成）。

## 拓扑与角色

```
FAR 节点（常驻守护，复用 08）                NEAR 节点（客户端）
┌────────────────────────────┐           ┌─────────────────────────────────────┐
│ contributor 贡献 DRAM slot │           │ 注册 HBM 张量 src/dst（本地端点）    │
│ （executor 自动以          │◄──────────│ device_copy：直发或 NPUGraph 重放    │
│  AI_CORE_INITIATE 加入池） │ RDMA WRITE │   ├─ graph: 整段轮转序列入图→重放K│
│                            │  (AICore   │   └─ direct: 直发 K 次 + 末次同步  │
│                            │   自发)    │ 每粒度 write/read 计时 + 回读校验   │
└────────────────────────────┘           └─────────────────────────────────────┘
```

## 使用能力
- `SMEMRA_DATA_OP_DEVICE_SCHEDULE` 修饰位（与 `DEVICE_RDMA` 组合）：池实体切为
  `HYBM_TYPE_AI_CORE_INITIATE`，trans 层建 AI 核 QP 并把 meta/QP/MR 表发布进固定
  GVA 窗口（物理上在 HBM 段固定区）
- 设备头 `smem_ralloc_aicore_base_rdma.h`：设备侧内联 RDMA 引擎（WQE 构造、doorbell、
  CQ 轮询、quiet），meta 窗口自发现
- `handle.device_copy`（地址直传、方向按地址自动判定、纯入队不同步；安装期 bisheng
  编译的随包内核，`dlopen` 惰性加载）
- `handle.device_copy_batch_v2`（**路由 DVA 单次提交**）：五个描述符数组（SoA：src/dst/size
  为 uint64、rank/write 为 uint32）驻留 HBM，kernel 从 GM 直读、**一次 launch 驱动整批**
  ——无 SEG_MAX 分段、无每 chunk 管线排空；host 侧不解引用描述符（**信任契约**：
  size ∈ (0,4G]、peerRank < rankCount 由调用方自负，错误路由安全退级为 WQE 跳过）；
  SQ 流控由内核子批 quiet 承担（2048 段/lane，SQ 环深 8192）；staging 须同 stream H2D
  且缓冲跨调用/图回放存活（流序保证复用无竞态）
- 用户注册 HBM（`handle.register`）作本地端点：池槽 GVA 仅作 device_copy 端点，
  **禁止 CPU 解引用**（`--enable-56bits-gva` 下 GVA 窗位于 2^55 以上、无 CPU 映射）
- NPUGraph 捕获与重放（`torch.npu.NPUGraph`），warmup 必须在捕获外完成
- extend 真值地址：FAR 槽地址取自 `extend_remote_mem` 返回的 `info["gva"]`

## 快速开始（跨节点手工分别启动）

```bash
# 1) FAR 节点启动守护（与 08 相同），等到 "... memory contributors serving (...)"
python3 memfabric_daemon.py --store tcp://<far_ip>:8588 --devs 0,1

# 2) NEAR 节点跑客户端（默认 direct 直发计时）
python3 memfabric_client.py --store tcp://<far_ip>:8588 --dev 1

#    图模式（捕获重放）/ 批量模式 / 组合模式
python3 memfabric_client.py --store tcp://<far_ip>:8588 --dev 1 --graph
python3 memfabric_client.py --store tcp://<far_ip>:8588 --dev 1 --batch
python3 memfabric_client.py --store tcp://<far_ip>:8588 --dev 1 --graph --batch

# 3) 停止守护
kill -TERM <daemon_pid>
```

## 生命周期
- 客户端 create → extend_remote → 逐粒度（register → warmup → 计时 → 校验 →
  unregister）→ destroy 全链路自清理，退出无需通知守护
- 守护生命周期与 08 完全一致（SIGTERM 干净停机、父 pid 自检、日志机制相同）
- 客户端每次运行以 `"w"` 截断重写 `log/near_dev{N}.log`，统计行同步回显终端
- 可连跑多轮（守护复用范式）：客户端退出后 FAR 实体在宽限（默认 5s，
  `MF_RALLOC_POOL_GRACE_SEC`）+ 断链检测内自毁，下一轮命中全新实体；若上一轮
  异常退出后立刻重跑仍撞上垂死实体，等待 ~10s 再跑即自愈，无需重启守护

## 参数

**memfabric_daemon.py**：贡献者骨架与 08 完全相同（`--store/--devs/--world/--rpc-port-base/--run-dir`），
端口默认与全用例统一（RPC `11100`、NIC `10005`），与客户端一致——各用例的 daemon/client 可互换复用。

**memfabric_client.py**

| 参数 | 默认 | 说明 |
|---|---|---|
| `--store` | 必填 | 守护进程的 store url |
| `--dev` | 必填 | 客户端使用的 NPU id |
| `--io-sizes` | 1M,2M,4M,8M | 拷贝粒度列表（K/M/G 后缀，各项须 4 字节对齐且 ≤ `--real-pool-size`） |
| `--batch-size` | 64M | 每粒度每方向单向拷贝总量，blocks = batch-size / 粒度（K/M/G 后缀） |
| `--real-pool-size` | 64M | 从 FAR 池取的远端槽字节数（须覆盖最大粒度且 ≤ `--max-pool-size`） |
| `--max-pool-size` | 4G | 池 DRAM 窗口（`ralloc.create` 的 `max_dram_size`，**必须 GB 对齐**） |
| `--graph` | 关 | 计时段捕获为 NPUGraph 并重放 `--replays` 次（默认直发不捕获） |
| `--replays` | 4 | graph/graph-batch 模式每计时段的完整图重放次数（计时总量 = replays × batch-size 每方向） |
| `--batch` | 关 | 计时走批量接口：单用＝一次直发（batch），配 `--graph`＝捕获提交并重放（graph-batch）。+fast 走 `device_copy_batch_v2`（描述符 HBM 暂存 + **单次 kernel 提交**），不带 fast 走 checked `device_copy_batch`（64 段/launch 分块） |
| `--fast` | 关 | 计时段走**显式路由快速路径**（单发 `device_copy` 传 `peer_rank`/`is_write`；批量经 `stage_batch_desc` 暂存 HBM 后走 `device_copy_batch_v2` 单次提交），C 层跳过每段地址预检；warmup 保持检查路径作正确性门。**契约**：路由与描述符内容（size ∈ (0,4G]、peerRank < rankCount）由调用方自负——错误路由安全退化为 WQE 跳过（内核 MR 查找 miss），仅能靠回读校验发现，不报同步错误；暂存 tensor 须跨调用/图回放存活。收益看 `submit W/R` 打印（提交墙钟） |
| `--world` | 512 | 同守护 |
| `--rpc-port-base` | 11100 | 同守护 |
| `--enable-56bits-gva` | 关 | 56-bit GVA 建池（GVA 窗位于 2^55 以上，槽地址仅作设备端点） |
| `--run-dir` | ./log | 客户端日志目录（`near_dev{dev}.log`） |

## 必要条件
- CANN ≥ 8.3.RC1（NPUGraph 与 bisheng 设备侧编译）
- **池必须带 HBM 窗口**：设备侧 meta 窗口（rank/QP/MR 上下文发布区）物理上在 HBM 段固定
  区，create 时 `max_hbm_size` 须非 0 且 GB 对齐（本用例固定 1G，仅为承载 meta，拷贝 slot
  在 DRAM）；缺 HBM 窗口会被 C 层前置拦截并报
  `DEVICE_SCHEDULE requires a nonzero max_hbm_size`
- **本用例不提交本地 DRAM 槽**：NEAR 侧端点全部为注册 HBM 张量（合法腿 `USER ↔ PEER`）；
  若所用版本的 join/QP/meta 发布路径隐含要求本地槽，会在 extend 或捕获阶段报错——见排障表
- 安装期 `install ralloc device rdma lib success`（bisheng 在位；失败会 WARNING 跳过，
  客户端 `device_copy` 将报 library not available）
- 指定 NPU 卡 device RDMA 链路 UP 且跨节点可达；客户端另需 torch/torch_npu

## 验收标准
- 客户端关键行依次出现：
  1. `device-scheduled pool created (DEVICE_RDMA | DEVICE_SCHEDULE, 56bits_gva=...)`
  2. `remote block from FAR rank <R> (gva=0x...)`
  3. 每个粒度：`size <S>: warmup round-trip OK (registered HBM <-> FAR slot, seed <n>)`
  4. 每个粒度：`size <S>: <B> blocks (direct)` / `<B> blocks/1 batch (batch)` /
     `<B> blocks/graph x <R> replays (graph|graph-batch)`，后随 `write X GB/s (Y us/block),
     read Z GB/s (W us/block) [round-trip OK]`
  5. `[client] all sizes OK, client finished cleanly`，exit 0
- 守护侧正常常驻、停机 `all contributors stopped cleanly`

## 判读
- 四模式跑**同一轮转地址序列**：graph/graph-batch 吞吐 = `replays × blocks × size / 计时`，
  direct/batch = `blocks × size / 计时`（计时均含末次 synchronize）
- **实测矩阵（2026-10-09 · N=4（MF_QPS_PER_PEER=4）· DVA 单次提交 · us/block，w / r）**

  graph-batch fast（重放 4 次）：

  | size | 2M | 4M | 8M | 16M | 32M | 64M |
  |---|---|---|---|---|---|---|
  | 1K | 0.64/0.59 | 0.59/0.57 | 0.57/0.46 | 0.51/0.45 | 0.49/0.45 | 0.46/0.46 |
  | 2K | 0.65/0.64 | 0.50/0.50 | 0.48/0.46 | 0.46/0.45 | 0.46/0.45 | 0.44/0.44 |
  | 4K | 0.91/0.74 | 0.56/0.55 | 0.51/0.49 | 0.49/0.47 | 0.48/0.46 | 0.46/0.46 |
  | 8K | 0.94/0.92 | 0.63/0.61 | 0.54/0.53 | 0.50/0.49 | 0.49/0.47 | 0.46/0.46 |
  | 16K | 1.67/1.60 | 1.16/1.11 | 0.90/0.89 | 0.82/0.78 | 0.73/0.73 | 0.70/0.70 |
  | 32K | 3.03/4.64* | 2.16/2.14 | 1.73/1.72 | 1.54/1.54 | 1.44/1.44 | 1.39/1.39 |
  | 64K | 6.53/6.31 | 4.57/4.51 | 3.60/3.55 | 3.13/3.13 | 2.92/2.90 | 2.79/2.79 |
  | 128K | 12.13/12.33 | 8.61/8.66 | 6.96/6.91 | 6.19/6.15 | 5.76/5.74 | 5.56/5.56 |

  batch fast（eager 直发）：

  | size | 2M | 4M | 8M | 16M | 32M | 64M |
  |---|---|---|---|---|---|---|
  | 1K | 0.63/0.57 | 0.58/0.54 | 0.55/0.56 | 0.55/0.55 | 0.55/0.45 | 0.49/0.44 |
  | 2K | 0.62/0.61 | 0.60/0.56 | 0.56/0.54 | 0.45/0.45 | 0.45/0.46 | 0.43/0.45 |
  | 4K | 0.65/0.62 | 0.62/0.59 | 0.65/0.57 | 0.47/0.44 | 0.44/0.45 | 0.46/0.46 |
  | 8K | 0.76/0.70 | 0.63/0.64 | 0.60/0.59 | 0.49/0.47 | 0.47/0.45 | 0.45/0.44 |
  | 16K | 1.20/1.01 | 0.94/0.90 | 0.81/0.79 | 0.73/0.71 | 0.70/0.69 | 0.68/0.68 |
  | 32K | 2.27/2.05 | 1.78/1.69 | 1.59/1.53 | 1.44/1.43 | 1.39/1.38 | 1.37/1.37 |
  | 64K | 4.34/4.06 | 3.49/3.40 | 3.14/3.08 | 2.91/2.88 | 2.77/2.76 | 2.73/2.72 |
  | 128K | 8.66/8.14 | 6.94/6.65 | 6.27/6.12 | 5.84/5.73 | 5.57/5.53 | 5.46/5.44 |

  \* 32K@2M read 单点离群（4 次重放噪声）；32M 列为独立复测刷新值，其余与首轮 ±2-3%
  复现。submit W/R 已塌缩至 0.03-0.06ms / 0.02-0.03ms（0.00-0.02 us/段）。

- **成本模型（DVA 定稿）**：`t ≈ max(0.45us, size / 23.5GB/s)`——1K-8K 落纯消息管线
  地板 0.43-0.49us（多 lane 并行摊薄后与消息大小无关），16K 起纯带宽受限（64M 档
  22.3-22.4 GB/s ≈ 200G 单口线速）。标定条件：200G RoCE、N=4、bisheng 内核；换链路
  速率或批量结构需重新拟合
- **eager 与 graph 首次打平**：单次提交消除了分段 launch 边界与中途 quiet 排空，
  batch fast 与 graph-batch fast 在 ≥16M 档 ±2%（eager 略优——省去图回放派发）；
  host 提交成本塌缩 ~60×（8192 段 submit W：SEG64 时代 1.94ms → DVA 0.03ms）
- **优化弧线（2K@4M 档，us/block）**：N=1 单 QP 2.10 → 多 QP N=4 0.80 → SEG_MAX=120
  实验 0.59 → **DVA 单次提交 0.50**（@64M 0.43），累计 **4.2-4.9×**。分段开销已由
  SEG_MAX 剂量实验（64 vs 32 vs 120）定价：每 64 段 chunk ≈ launch + quiet 排空
  9.3us——该机制已由 DVA 单次提交移除
- **推荐档位**：`MF_QPS_PER_PEER=4` + `--batch-size ≥32M`（小 IO 地板与每图固定项
  摊销兼得；2M/4M 小 batch 档仍受每图固定成本影响，大 IO 档影响可忽略）
- 大消息（≥1M）各模式均 ~88-89% 线速（200G），无优化空间。设备侧 doorbell 批量化
  （一次 commit 覆盖整段 fill）**已试并回退**：实测各粒度劣化 ~6-9%——每段即时 ring
  维持内核↔NIC 流水重叠，WQE 构造本就藏在 wire 之下，批量化反而把 fill 串行前置于传输
- **宿主侧剩余靶点 = `SmemRallocDeviceSegCheck`**：checked 批量路径每段多次全量
  alloc-range 查询；`--fast`（显式路由/DVA）跳过全部逐段预检。收益看结果行的
  `submit W/R` 打印（提交墙钟），**稳态吞吐不受影响**（host 超前、设备 wire-bound、
  graph-batch 重放零 host）
- 单边提交 + 卡内 quiet，吞吐参考受消息粒度与 QP 深度约束，本用例验证的是
  **可捕获性、正确性与开销结构**，不是极限带宽
- verify 失败但 warmup 成功：优先怀疑计时期间链路/对端异常，`log/far_dev{N}.log`
  找 CQE 状态打印

## 多 QP（MF_QPS_PER_PEER）

设备侧每对 rank 建 N 条 QP 连接（N=env 值，钳制 [1,4]，默认 1 = 原单 QP 行为），内核
批量任务均匀分发到各 lane 并行：

- **连接拓扑**：全部 lane 连同一 `(ip, port)`，以 tag 区分（lane 0 = 原空 tag 主连接，
  lane i≥1 = `mf_q2_<clientRank>_<i>`，client 侧 rank 生成、双端一致——与 HCCL
  `HcclSocketManager` 生产接线同款；本 hccp 构建经 route-A v5 实验验证支持）
- **内核形态**：批量内核 `blockDim = N`，block b 独占 lane b（SQ head 是单生产者索引，
  1:1 绑定免锁）；段 j 走 block `j % N`，确定性分发（graph 重放一致）；每 block 末尾
  只 quiet 自己 lane 的 distinct peer——N 路 quiet 并行等待，**小 IO 区每 chunk 的串行
  quiet（~1.8us/段）是主要收益靶点**
- **运行**：双端同设，如 `MF_QPS_PER_PEER=2 python3 memfabric_daemon.py ...` /
  `MF_QPS_PER_PEER=2 python3 memfabric_client.py ...`（不全设 = 连接失败，all-or-nothing：
  任一 lane 失败即中止启动，不留死 lane）
- **判读**：`multi-QP enabled`（Startup）/ `multi-QP client|server ... lane i ... tag`
  （建立）；性能对比看 2K-16K 小 IO 区每块时间（单 QP 基线 1.65-2.03us/块）与
  128K-1M 大 IO 吞吐（应持平，已线速）；direct 模式单发不受益（单段无分发）
- **与 DVA 单次提交的组合**（`device_copy_batch_v2`）：内核同样 `blockDim = N`、
  block b 独占 lane b；描述符改由 kernel 从 GM 直读（SoA 五数组，基址随 launch 参数
  下发）；lane 内在途 WQE 以 **2048 段/lane 为子批做 quiet**（SQ 环深 8192，实际授予值
  经 QP 表 `depth` 字段下发内核）——任意批量安全，不再受 SEG_MAX 分段约束
- 历史实验门控 `MF_SOCKS_PER_PEER` 已由正式实现取代并移除

## 排障（精简）
| 症状 | 处置 |
|---|---|
| create 报 `must align GB` | 池窗口（`--max-pool-size`）未 GB 对齐：VMM 段硬性要求，默认 4G 已满足，自定义时注意 |
| create 报 `DEVICE_SCHEDULE requires a nonzero max_hbm_size` | 设备调度依赖 HBM 窗口承载 meta：本用例固定 1G，自行改造时须 GB 对齐 |
| create/extend 报 `DEVICE_SCHEDULE without DEVICE_RDMA` | 组合位校验：SCHEDULE 必须与 DEVICE_RDMA 同用 |
| extend/join/捕获阶段报与本地槽相关错误 | 本用例依赖“注册 HBM 可作唯一本地端点”成立；若报错说明该版本设备调度路径隐含要求本地 DRAM 槽——记录现象反馈，并暂用 09 形态（`extend_local_mem` + 槽到槽）对比 |
| device_copy 返回非 0 且日志报 library not available | 安装期 bisheng 缺失或编译失败：确认 `bisheng` 在 PATH、重跑 install.sh，检查 `lib64/libmf_smem_ralloc_device_rdma.so` 是否存在 |
| device_copy 报 `copy endpoint falls neither into the pool device window nor a registered user region` | 地址不是本池窗口 GVA（须用 extend 返回的 `info["gva"]`）或用户内存未 register |
| device_copy 报 `needs one local endpoint ... and one peer pool slot` | 组合非法：本用例只允许 张量↔FAR 槽 两腿；张量↔张量或槽↔槽（本端）不支持 |
| 捕获阶段报错/崩溃 | warmup 是否在捕获外执行过（本用例每粒度已内置）；torch_npu 版本需支持 `torch.npu.NPUGraph` |
| `pool is not device-scheduled` | `data_op_type` 未带 `DEVICE_SCHEDULE`（本用例已内置，自行改造时注意） |
| device_copy_batch_v2 返回错误且日志报 `batch descriptor array address is 0` | 五个描述符数组地址有 0 值：检查 staging tensor 的 `data_ptr()` 是否正确传入五个参数 |
| 回读校验失败且数据陈旧/为空 | staged 描述符缓冲被提前回收（tensor 删除/出作用域）——描述符 tensor 必须跨调用与图回放存活；staging H2D 与提交须在同一 stream |
| 其余与 08 相同 | 参照 08_far_daemon_near_client 排障表 |
