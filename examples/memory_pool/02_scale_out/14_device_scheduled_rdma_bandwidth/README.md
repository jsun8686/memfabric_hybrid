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
  - `--batch` **batch**：同序列**一次 `device_copy_batch` 直发提交**（C 层按 64 段/launch
    分块、每 distinct peer 仅 1 次 quiet）——与 direct 对照即批量接口对小消息固定开销
    （launch + quiet）的摊薄收益；
  - `--graph --batch` **graph-batch**：捕获**一次 `device_copy_batch` 提交**入图并重放——
    重放零 host 参与**且收敛摊薄**；与 graph 同量对照即 quiet 摊薄收益，与 batch 对照即
    直发提交开销。

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
| `--batch` | 关 | 计时用 `device_copy_batch`（C 层 64 段/launch 分块、每 peer 一次 quiet）：单用＝一次直发（batch），配 `--graph`＝捕获提交并重放（graph-batch） |
| `--fast` | 关 | 计时段走**显式路由快速路径**（`device_copy(_batch)` 传 `peer_rank`/`is_write`（batch 传平行数组），C 层跳过每段地址预检，只留常数项检查）；warmup 保持检查路径作正确性门。**契约**：路由由调用者自负——错误路由安全退化为 WQE 跳过（内核 MR 查找 miss），仅能靠回读校验发现，不报同步错误；收益看 `submit W/R` 打印（提交墙钟） |
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
- **实测矩阵（2026-09-29，write GB/s，SEG_MAX=64）**：

  | size | graph-batch | batch | graph* | direct | ex08 host |
  |---|---|---|---|---|---|
  | 128K | **19.57** | 16.32 | 7.95 | 8.33 | 4.60 |
  | 256K | **21.73** | 18.99 | 11.80 | 12.17 | 7.66 |
  | 512K | **21.76** | 20.58 | 15.35 | 15.74 | 10.34 |
  | 1M | **21.93** | 21.45 | 17.59 | 18.47 | 14.17 |
  | 8M | 21.99 | **22.24** | 21.47 | 21.88 | 21.07 |

  \* graph 列沿用上一轮数据：该模式单拷贝直发、不经分块路径，SEG_MAX 不影响。
  SEG_MAX 16→64 增量（batch/graph-batch 128K +7.5%/+3.7%，256K +7.6%/+5.0%）
  = 每 chunk 内核（launch+quiet ≈5us）的摊薄倍数 4×，与机制吻合。

- **128K 每块固定开销阶梯**（wire ≈5.8us 之上加价，三层优化的量化因果链）：
  `direct ~8.8us`（launch+quiet+WQE）→ `batch ~1.7us`（launch/quiet 摊薄 64 段）→
  `graph-batch ~0.4us`（重放替代直发提交；read 已达线速 21.15 GB/s）——小消息累计
  **4.26×**（host 4.60 → 19.57 GB/s）
- **graph-batch vs graph** = quiet 摊薄（单拷贝内核自带 quiet，图内逐拷贝串行收敛；batch
  每 distinct peer 仅 1 次），128K **+146%**；**graph-batch vs batch** = 重放替代直发提交
  的 host launch 路径，128K **+20%**；**batch vs direct** = 每块恒省 ~7.2us（与消息
  大小无关，即 launch+quiet 固定成本）
- 大消息（≥4M）各模式均 ~88-89% 线速（200G），无优化空间。设备侧 doorbell 批量化
  （一次 commit 覆盖整段 fill）**已试并回退**：实测 batch/graph-batch 各粒度劣化 ~6-9%
  （128K 6.24→6.69us/块）——每段即时 ring 维持内核↔NIC 流水重叠，WQE 构造本就藏在
  wire（~5.8us/块）之下，批量化反而把 fill 串行前置于传输
- **宿主侧剩余靶点 = `SmemRallocDeviceSegCheck`**：检查路径每段多次全量 alloc-range 查询
  （batch 512 段提交 ~数 ms）；`--fast` 走显式路由（用户自报 `peer_rank`/`is_write`，
  C 层只留常数项检查）。收益看结果行的 `submit W/R` 打印（提交墙钟），**稳态吞吐
  不受影响**（host 超前、设备 wire-bound、graph-batch 重放零 host）
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
| 其余与 08 相同 | 参照 08_far_daemon_near_client 排障表 |
