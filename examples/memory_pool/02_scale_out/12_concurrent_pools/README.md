# 12_concurrent_pools

## 场景
**多池并发创建**：N 个 worker 各建**独立 pool**（不同 pool id，每次 `extend_remote_mem`
驱动各自的 JOIN_ALLOC create 分支），并在 barrier 对齐后**同一瞬间**齐发扩容——placement
master 在毫秒窗内连续放行，多个 create 分支并发命中同一 FAR 贡献节点。

- 贡献侧设备打开（`PrepareOpenDevice`）必须对并发 create **串行化**：后到者等待先到者的
  `RaRdevInit` 注册句柄后走复用路径（`Had prepared device and get rdmaHandle success`），
  而不是并发跑 `RaInit`（libra 会以 double-init 拒绝，其 hccl-group 回退也随之失败）；
- 本例即该竞态的复刻与回归：历史日志中同节点第二个 create 曾以
  `Hccp Init RA failed: 328002` → `HcclCommInitClusterInfoMemConfig failed ret:19` 失败，
  依赖客户端 5s 重试换点才恢复；
- 每池一块远端 DRAM block，host 发起 `copy_data` 双向往返（worker 唯一 pattern）作数据面
  自证，随后自毁。

## 拓扑与角色

```
FAR 节点（常驻守护，复用 09/10/11 形态）      NEAR 节点（客户端单控制器）
┌────────────────────────────────┐          ┌────────────────────────────────────┐
│ contributor 贡献 DRAM slot      │          │ N 个 worker 进程（spawn，各占一卡） │
│  ├─ pool 120 create（先到）     │◄─────────│  各建独立 pool 120..123（本地建窗） │
│  ├─ pool 121 create（并发到达） │ JOIN_    │  barrier 对齐 ──► 同一瞬间          │
│  └─ ...（须串行化设备打开）     │  ALLOC   │  extend_remote_mem 齐发           │
│  后到者：等待 ─► 复用 rdma 句柄 │ 齐发     │  copy_data 往返验证 ─► destroy     │
└────────────────────────────────┘          └────────────────────────────────────┘
```

## 使用能力
- placement master 并发放行：in-flight 记账使连续 grant 的负载视图准确（grant 存活至落地，
  失败主动 NACK），N 个请求得到均匀分布
- 同节点并发 create 的设备打开串行化：`PrepareOpenDevice` 进程级互斥，后到者重查
  `RaRdevGetHandle` 走复用路径，不再触发 `RaInit` double-init / hccl 建组失败
- 独立 pool id 并存：FAR 侧每池一个 entry/entity，动态 join 各自合并
- host 发起 `copy_data`（`DEVICE_RDMA`，无 `DEVICE_SCHEDULE`）：注册张量端点 + 远端
  block 双向往返

## 快速开始（跨节点手工分别启动）

```bash
# 1) FAR 节点启动守护（与 09/10/11 相同形态），等到 "... memory contributors serving (...)"
python3 memfabric_daemon.py --store tcp://<far_ip>:8588 --devs 0,1

# 2) NEAR 节点跑客户端：N 池齐建 ─► 并发 extend ─► 各自往返验证 ─► 汇总
python3 memfabric_client.py --store tcp://<far_ip>:8588 --dev 0

# 3) 停止守护
kill -TERM <daemon_pid>
```

## 生命周期
- 客户端控制器 spawn N 个 worker → 各 worker `ralloc.create(pool_base+i)`（本地建窗入组，
  互不相干）→ barrier 对齐 → 齐发 `extend_remote_mem`（master 连续放行，create 分支并发
  到达贡献节点）→ 每池 `copy_data` 往返验证 → `destroy` 自清理退出
- 守护生命周期与 09/10/11 完全一致
- 客户端每次运行以 `"w"` 截断重写 `log/near_summary.log` 与 `log/near_worker{i}.log`

## 参数

**memfabric_daemon.py**：与 09/10/11 完全相同（`--store/--devs/--world/--rpc-port-base/--run-dir`）。

**memfabric_client.py**

| 参数 | 默认 | 说明 |
|---|---|---|
| `--store` | 必填 | 守护进程的 store url |
| `--dev` | 必填 | 所有 worker 共用的 NPU id |
| `--workers` | 4 | 并发建池数（须 ≥2，即本例的并发度） |
| `--size` | 1M | 往返验证 pattern 字节数（K/M/G 后缀，须 4 字节对齐且 ≤ block-size） |
| `--block-size` | 64M | 每池远端 block 字节数（K/M/G 后缀） |
| `--max-pool-size` | 4G | 池 DRAM 窗口（**必须 GB 对齐**，须 ≥ block-size） |
| `--pool-base` | 120 | 首个 pool id，worker i 用 `pool-base+i` |
| `--world` | 512 | 同守护 |
| `--rpc-port-base` | 11110 | 同守护 |
| `--run-dir` | ./log | 客户端日志目录（`near_summary.log` + `near_worker{i}.log`） |

## 必要条件
- 在 09/10 的必要条件之上：
- **两端版本一致**：并发 create 串行化为 host 库行为，不得与旧版守护混跑（旧行为依赖
  客户端重试兜底，FAR log 会出现 328002/19 报错）
- 连续重跑前确认 FAR 侧旧池已 reap（pool-empty 后默认 grace + 一个上报周期，约 30s），
  或直接换 `--pool-base` 避开残留 pool id

## 验收标准
- 客户端关键行依次出现：
  1. `<N> workers spawned, pools <base>..<base+N-1> building`
  2. 每 worker 一行：`pool <id> block on rank <r> (gva=0x...), round-trip OK, extend attempts: <k>`
  3. `placement spread: rank 0 -> 2 block(s), rank 1 -> 2 block(s)`（4 worker 的期望分布）
  4. `first-try extend success: <n>/<N>`
  5. `(<N>/<N>) 12_concurrent_pools: concurrent multi-pool create OK`，exit 0
- **FAR 侧（硬性）**：`far_dev*.log` 中
  - **无** `Hccp Init RA failed`（328002 竞态）
  - **无** `HcclCommInitClusterInfoMemConfig failed`
  - 同 rank 第二个 create 起出现 `Had prepared device and get rdmaHandle success`（串行等待
    后的复用路径），且每池一条 `join alloc success`
- 守护侧正常常驻、停机 `all contributors stopped cleanly`

## 判读
- `first-try` 计数不是硬断言：master 视图时序可致个别 worker 重试一次（客户端 5s 重试是
  设计内行为）；**FAR log 的 328002/19 报错才是失败信号**（出现即串行化未生效或版本混布）
- `round-trip mismatch`：block 被别的 worker 混写（不应发生——各池独立 block；出现时查
  FAR log 的 placement 记录确认 gva 归属）
- 分布偏向一端（如 3:1）：看 master `placement granted` 的 `load:` 序列——非单调增长说明
  记账回退（参照 07 例判读）

## 排障（精简）
| 症状 | 处置 |
|---|---|
| worker 卡在 barrier 直到超时 | 某 worker 本地建池慢/失败，看对应 `near_worker{i}.log` |
| `extend failed ... retrying` 反复 | 守护是否存活、FAR 窗口余量（`--max-pool-size` ≥ block）；连接类错误看 store 可达性 |
| worker 日志出现 `328002` / `ret:19` 级联 | 守护为旧版本（无串行化）：两端同步到同 commit 重编重启后重跑 |
| 重跑报 pool 已存在类错误 | 等 FAR reap（~30s）或换 `--pool-base` |
| 其余与 09 相同 | 参照 09_near_device_scheduled_rdma 排障表 |
