# 11_pool_multi_slice

## 场景
**池多 slice 扩容下的设备侧调度 RDMA**：同一个池向两侧各扩容两次
（`extend_local_mem` ×2 + `extend_remote_mem` ×2），每个 slice 独立注册 MR，使每个
rank 的 `ConnectRankInfo.memoryMap` 持有两条 MR 记录——设备可见的 QP/MR 表必须同时
暴露两者（`MR_SLOTS_PER_RANK = 8`，每 rank 8 个 MR 槽，设备侧按地址区间匹配取槽）。

- 09/10 例每 rank 只有一个 block MR（kernel 表单槽直取即可）；本用例验证扩容后
  kernel 对 WQE **双端点**各自选对覆盖槽；
- 端到端流：扩容 2+2 slice → host 侧灌两套不同 pattern 进本地 slice → warmup 双写
  → NPUGraph 捕获**两条** WRITE（各走一个 slice）→ 重放 → 本地清零 → 读回两个
  FAR slice → 逐槽比对（槽选串了 pattern 会交叉 → 校验失败）→ destroy；
- 双 pattern 交叉校验是核心断言：证明远端 rkey 与本地 lkey 都来自正确的 MR 槽。

## 拓扑与角色

```
FAR 节点（常驻守护，复用 09/10）         NEAR 节点（客户端）
┌────────────────────────────┐          ┌─────────────────────────────────────┐
│ contributor 贡献 DRAM slot │          │ 本地 slice 1/2（64M，pattern 源/    │
│  ├─ slice 1: 64M（独立 MR）│◄─────────│   读回落点）                         │
│  └─ slice 2: 64M（独立 MR）│ RDMA     │ NPUGraph: device_copy ×2             │
│  （每 rank 2 条 memoryMap）│  WRITE/  │   ├─ 槽 0/1 区间匹配取 rkey/lkey     │
│                            │  READ    │ 捕获 1 次（双写）→ replay K 次       │
└────────────────────────────┘          └─────────────────────────────────────┘
```

## 使用能力
- 同一池多次 `extend_local_mem` / `extend_remote_mem`：每次 extend 产生独立 slice 与
  MR，`memKeys` 随 slice 增长，对端经 dynamic join 合并进 memoryMap
- 设备侧 MR 表多槽化（`MR_SLOTS_PER_RANK = 8`）：`FillQpInfo` 按 rank 填前 8 槽
  （地址降序，未用槽保持零，`addr == 0` 永不匹配），kernel `post_send` 对远/本端点
  均做 8 槽区间匹配；超 8 条 host 打 WARN 并按地址截断（host 路径 copy 不受影响）
- `device_copy` 预检天然兼容多 slice：`GetMemSizeByRank` 返回已分配 ranges 的最大
  extent，第二 slice 落在 `[slotBase, slotBase + extent)` 内

## 快速开始（跨节点手工分别启动）

```bash
# 1) FAR 节点启动守护（与 09/10 相同），等到 "... memory contributors serving (...)"
python3 memfabric_daemon.py --store tcp://<far_ip>:8588 --devs 0

# 2) NEAR 节点跑客户端：建池 → 扩容 2+2 → 灌 pattern → warmup → 捕获 → 重放 → 交叉校验
python3 memfabric_client.py --store tcp://<far_ip>:8588 --dev 0

# 3) 停止守护
kill -TERM <daemon_pid>
```

## 生命周期
- 客户端 create → extend ×2（本地）+ extend ×2（远端，断言两段 gva 不重叠且同
  FAR rank）→ host 灌 pattern → warmup → 捕获双写 → 重放 → 清零本地 → 读回双远端
  slice 比对 → destroy 全链路自清理，退出无需通知守护
- 守护生命周期与 09/10 完全一致
- 客户端每次运行以 `"w"` 截断重写 `log/near_dev{N}.log`

## 参数

**memfabric_daemon.py**：与 09 完全相同（`--store/--devs/--world/--rpc-port-base/--run-dir`）。

**memfabric_client.py**

| 参数 | 默认 | 说明 |
|---|---|---|
| `--store` | 必填 | 守护进程的 store url |
| `--dev` | 必填 | 客户端使用的 NPU id |
| `--size` | 1M | 每套 pattern/每次单边拷贝的字节数（K/M/G 后缀，须 4 字节对齐） |
| `--replays` | 8 | 捕获后重放次数（计时段，每次含两条 WRITE） |
| `--block-size` | 64M | 每次 extend 提交的 DRAM slot 字节数（两侧各两次，须 ≥ size） |
| `--max-pool-size` | 4G | 池 DRAM 窗口（**必须 GB 对齐**，须容纳 2 × block-size） |
| `--world` | 512 | 同守护 |
| `--rpc-port-base` | 11105 | 同守护 |
| `--run-dir` | ./log | 客户端日志目录（`near_dev{dev}.log`） |

## 必要条件
- 在 09/10 的必要条件之上：
- **两端版本一致**：MR 表多槽布局（`[rankCount × 8]`）为 host 库与设备头的硬契约
  （`dl_hccp_def.h`、`smem_ralloc_aicore_base_rdma.h`、`smem_shm_aicore_base_rdma.h`
  同包发布），不得混版本运行
- 每 rank slice 数 > 8 时第 9 个起设备侧不可见（host WARN + 截断），host 路径
  copy 仍可用；需要更多槽时扩 `MR_SLOTS_PER_RANK` 并整包同步三处定义

## 验收标准
- 客户端关键行依次出现：
  1. `device-scheduled pool created (DEVICE_RDMA | DEVICE_SCHEDULE)`
  2. `pool grown to 2+2 slices: FAR rank <R> slots [0x..., 0x...)`（两段 gva 不重叠）
  3. `patterns loaded: <size> bytes each (seeds ...)`
  4. `warmup writes OK (kernel library loaded, both slices reachable)`
  5. `graph captured: 2 x <size> byte device-scheduled WRITEs, one per slice`
  6. `<replays> replays done: ... GB/s device-scheduled`
  7. `verify OK: both FAR slices carry their own pattern`（交叉校验通过 = 槽选对）
  8. `[client] pool multi-slice device-scheduled copies under NPU graph finished cleanly`，exit 0
- 守护侧正常常驻、停机 `all contributors stopped cleanly`；FAR log **无**
  `only the first 8 are visible` 截断 WARN（2 slice 远小于 8 槽）

## 判读
- replay 吞吐语义与 09/10 相同（验证可捕获性与正确性，非极限带宽；每次 replay 搬
  2 × size 字节）
- extend 第二次失败：ralloc 层对同池多次扩容的限制（看 `extend_*` 返回与 C 层日志）
- warmup 即失败：第二 slice 的预检/槽匹配问题，看 `copy range falls outside ...`
  或 user MR 表 errCode 打印
- verify 失败但 warmup 成功：两 pattern 交叉 = kernel 槽匹配错位；完全错数据 =
  replay 期间链路异常，`log/far_dev{N}.log` 找 CQE 状态打印

## 排障（精简）
| 症状 | 处置 |
|---|---|
| `extend_remote_mem` 第二次返回失败 | 确认 FAR 侧池 DRAM 窗口余量（`--max-pool-size` ≥ 2 × block）与 daemon 存活 |
| device_copy 报 `copy range falls outside the committed slot` | 第二 slice 未落进 extent：查 FAR log 的 placement/注册记录，确认两次 extend 都成功 |
| verify 报 pattern mismatch | pattern 交叉 = MR 槽数据错位（应回看 FillQpInfo 日志）；全零/脏数据 = 链路或 CQE 异常 |
| FAR log 出现 `only the first 8 are visible` | slice 超槽（本用例不应出现）：检查是否有其他客户端共用该池 |
| 其余与 09 相同 | 参照 09_near_device_scheduled_rdma 排障表 |
