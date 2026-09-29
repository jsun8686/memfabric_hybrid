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
- 计时模式可开关，两模式跑**同一槽轮转拷贝序列**（同地址、同字节数），差值即图捕获收益：
  - 默认 **graph**：每方向捕获**一张完整图**——图内含整个轮转序列（blocks 份拷贝，默认
    `batch-size = real-pool-size` 时恰好全槽每个地址各写一遍），重放 `--replays` 次；
    即真实场景形态（完整图中内嵌整段槽轮转通信），重放期间 host 零参与；
  - `--no-graph` **direct**：同序列循环直发 `device_copy`（纯入队不同步），末次
    synchronize 计入——同语义基线。

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

# 2) NEAR 节点跑客户端（graph 模式，默认）
python3 memfabric_client.py --store tcp://<far_ip>:8588 --dev 1

#    或 direct 模式（直发计时，量化图捕获收益）
python3 memfabric_client.py --store tcp://<far_ip>:8588 --dev 1 --no-graph

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
| `--no-graph` | 关 | 计时用直发 `device_copy`（轮转偏移 + 末次同步），不捕获 NPUGraph |
| `--replays` | 4 | graph 模式每计时段的完整图重放次数（计时总量 = replays × batch-size 每方向） |
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
  4. 每个粒度：`size <S>: <B> blocks (direct)` 或 `<B> blocks/graph x <R> replays (graph)`，
     后随 `write X GB/s (Y us/block), read Z GB/s (W us/block) [round-trip OK]`
  5. `[client] all sizes OK, client finished cleanly`，exit 0
- 守护侧正常常驻、停机 `all contributors stopped cleanly`

## 判读
- 两模式跑**同一轮转地址序列、同字节总量**：graph 吞吐 = `replays × blocks × size / 计时`，
  direct 吞吐 = `blocks × size / 计时`（计时均含末次 synchronize）
- 两模式差值 = host 逐次入队开销的消除量（图捕获收益）；消息粒度越小收益应越明显
- **若 graph 仍不优于 direct**：瓶颈不在 host 入队，而在随包设备内核的作业形态
  （若每拷贝自带 quiet，图内仍逐拷贝串行收敛）——那是设备内核层面的下一个优化靶
- 单边提交 + 卡内 quiet，吞吐参考受消息粒度与 QP 深度约束，本用例验证的是
  **可捕获性、正确性与开销结构**，不是极限带宽
- verify 失败但 warmup 成功：优先怀疑计时期间链路/对端异常，`log/far_dev{N}.log`
  找 CQE 状态打印

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
