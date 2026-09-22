# 09_near_device_scheduled_rdma

## 场景
**设备侧（AICore 卡上）调度的 RDMA 数据收发 + NPU Graph 捕获重放**（PoC 用例，08 小幅改造）：

- 常规 ralloc 拷贝（`copy_data`）由 host 发起：每次拷贝都有 host 交互，**无法被 NPU Graph
  捕获**，图模式下通信与计算无法重叠；
- 本用例的 DRAM 池以 `DEVICE_RDMA | DEVICE_SCHEDULE` 创建（拓扑与 08 相同，daemon 原样
  复用）：trans 层改建 AI 核 QP 并把 meta/QP/MR 上下文发布进固定设备 meta 窗口，随包内核
  （安装期 bisheng 编译的 `libmf_smem_ralloc_device_rdma.so`）在 AICore 上**自发现上下文、
  自发 WQE、自收 CQ、自 quiet**，全程零 host 交互；
- host 侧 `device_copy`（对齐 `copy_data` 家族：地址直传、方向自动判定）只做一次内核
  入队 → 整个拷贝作业（单边 WRITE/READ + quiet）可被 NPUGraph 捕获，**捕获一次、重放
  K 次**，重放期间 host 完全不参与。

## 拓扑与角色

```
FAR 节点（常驻守护，复用 08）                NEAR 节点（客户端）
┌────────────────────────────┐           ┌─────────────────────────────────────┐
│ contributor 贡献 DRAM slot │           │ 本地 DRAM slot（SVM，host 可写）     │
│ （executor 自动以          │◄──────────│ NPUGraph: device_copy               │
│  AI_CORE_INITIATE 加入池） │ RDMA WRITE │   ├─ AICore 单边写 + quiet          │
│                            │  (AICore   │ 捕获 1 次 → replay K 次 → 读回校验  │
│                            │   自发)    │                                     │
└────────────────────────────┘           └─────────────────────────────────────┘
```

## 使用能力
- `SMEMRA_DATA_OP_DEVICE_SCHEDULE` 修饰位（与 `DEVICE_RDMA` 组合）：池实体切为
  `HYBM_TYPE_AI_CORE_INITIATE`，trans 层建 AI 核 QP（FixedRanks 全互联）并把
  meta/QP/MR 表发布进固定 GVA 窗口；池窗口可在 DRAM 或 HBM（本用例用 DRAM，与 08 对齐）
- 设备头 `smem_ralloc_aicore_base_rdma.h`：设备侧内联 RDMA 引擎（WQE 构造、doorbell、
  CQ 轮询、quiet），meta 窗口自发现
- `handle.device_copy` / `handle.device_copy_batch` / `handle.get_entity_id`
  （对齐 `copy_data` / `copy_data_batch` 家族：地址直传 + 方向按地址自动判定 + 批量一次
  提交；安装期编译的随包内核，`dlopen` 惰性加载；纯入队不同步）
- 本地端还支持用户注册内存（`handle.register` 后 NPU 张量地址可直接作端点，设备侧查
  用户 MR 表取 lkey，本用例不演示）
- NPUGraph 捕获与重放（`torch.npu.NPUGraph`），warmup 必须在捕获外完成（首次
  device_copy 会 `dlopen` + 加载内核，捕获中非法）

## 快速开始（跨节点手工分别启动）

```bash
# 1) FAR 节点启动守护（与 08 相同），等到 "... memory contributors serving (...)"
python3 memfabric_daemon.py --store tcp://<far_ip>:8587 --devs 0,1

# 2) NEAR 节点跑客户端：建池 → 捕获 → 重放 → 回读校验
python3 memfabric_client.py --store tcp://<far_ip>:8587 --dev 1

# 3) 停止守护
kill -TERM <daemon_pid>
```

## 生命周期
- 客户端 create → extend（本地 + 远端 DRAM slot）→ warmup → 捕获 → 重放 → 回读校验 →
  destroy 全链路自清理，退出无需通知守护
- 守护生命周期与 08 完全一致（SIGTERM 干净停机、父 pid 自检、日志机制相同）
- 客户端每次运行以 `"w"` 截断重写 `log/near_dev{N}.log`，统计行同步回显终端

## 参数

**memfabric_daemon.py**：与 08 完全相同（`--store/--devs/--world/--rpc-port-base/--run-dir`）。

**memfabric_client.py**

| 参数 | 默认 | 说明 |
|---|---|---|
| `--store` | 必填 | 守护进程的 store url |
| `--dev` | 必填 | 客户端使用的 NPU id |
| `--size` | 1M | 每次单边 RDMA 拷贝的字节数（K/M/G 后缀） |
| `--replays` | 8 | 捕获后重放次数（计时段，放大由重放承担） |
| `--block-size` | 64M | 两侧各提交的 DRAM slot 字节数（须 ≥ 2×size：slot 布局 `[pattern \| verify]`） |
| `--max-pool-size` | 4G | 池 DRAM 窗口（`ralloc.create` 的 `max_dram_size`，**必须 GB 对齐**） |
| `--world` | 512 | 同守护 |
| `--rpc-port-base` | 11100 | 同守护 |
| `--run-dir` | ./log | 客户端日志目录（`near_dev{dev}.log`） |

## 必要条件
- CANN ≥ 8.3.RC1（NPUGraph 与 bisheng 设备侧编译）
- 安装期 `install ralloc device rdma lib success`（bisheng 在位；失败会 WARNING 跳过，
  客户端 `device_copy` 将报 library not available）
- 指定 NPU 卡 device RDMA 链路 UP 且跨节点可达；客户端另需 torch/torch_npu
- 两端各贡献一个 slot 后池成员即全员到位（FixedRanks 静态全互联，本用例 2 成员拓扑天然满足）

## 验收标准
- 客户端关键行依次出现：
  1. `device-scheduled pool created (DEVICE_RDMA | DEVICE_SCHEDULE)`
  2. `warmup round-trip OK (kernel library loaded, meta window reachable)`
  3. `graph captured: 1 x <size> byte device-scheduled WRITE + quiet, no host interaction inside`
  4. `<replays> replays done: ... GB/s device-scheduled`
  5. `verify OK: far rank <R> slot matches the pattern`
  6. `[client] device-scheduled RDMA under NPU graph finished cleanly`，exit 0
- 守护侧正常常驻、停机 `all contributors stopped cleanly`
- 可连跑多轮客户端（守护复用范式）

## 判读
- replay 吞吐 = `replays × size / 计时`（计时含末次 synchronize）；单边 WRITE 串行提交 +
  卡内 quiet，吞吐参考受消息粒度与 QP 深度约束，本用例验证的是**可捕获性与正确性**，
  不是极限带宽
- verify 失败但 warmup 成功：优先怀疑 replay 期间链路/对端异常，`log/far_dev{N}.log`
  找 CQE 状态打印

## 排障（精简）
| 症状 | 处置 |
|---|---|
| create 报 `must align GB` | 池窗口（`--max-pool-size`）未 GB 对齐：VMM 段硬性要求，默认 4G 已满足，自定义时注意 |
| device_copy 返回非 0 且日志报 library not available | 安装期 bisheng 缺失或编译失败：确认 `bisheng` 在 PATH、重跑 install.sh，检查 `lib64/libmf_smem_ralloc_device_rdma.so` 是否存在 |
| device_copy 报 `copy endpoint falls neither into the pool device window nor a registered user region` | 地址不是本池窗口 GVA（slot 地址用 `get_mem_ptr_by_rank(rank, HOST)` 取）或用户内存未 register |
| device_copy 报 `needs one local endpoint ... and one peer pool slot` | 组合非法：两端同为本地/远端、或用户内存 × 用户内存（远端用户内存暂不支持） |
| 捕获阶段报错/崩溃 | warmup 是否在捕获外执行过（本用例已内置）；torch_npu 版本需支持 `torch.npu.NPUGraph` |
| `pool is not device-scheduled` | `data_op_type` 未带 `DEVICE_SCHEDULE`（本用例已内置，自行改造时注意） |
| create/extend 报 `DEVICE_SCHEDULE without DEVICE_RDMA` | 组合位校验：SCHEDULE 必须与 DEVICE_RDMA 同用 |
| 其余与 08 相同 | 参照 08_far_daemon_near_client 排障表 |
