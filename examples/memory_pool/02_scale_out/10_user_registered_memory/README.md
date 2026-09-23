# 10_user_registered_memory

## 场景
**用户注册的 NPU HBM 张量作为设备侧调度 RDMA 拷贝端点**（09 + `register()`，P1 落地验证）：

- 09 例的本地端点只能是池 slot（DRAM 窗口）；torch 侧数据进出池需要额外一次卡内
  staging copy；
- 本用例把 torch 张量地址经 `handle.register(addr, size)` 注册进池（P1：**仅支持 NPU
  管理的 HBM 内存**，host 在注册入口按 HBM 地址段直接拒绝其他地址），HBM MR 满足
  `regAddress == addr` 恒等，随包内核（AICore）在用户 MR 表中取 lkey 与地址直接收发；
- 端到端流：注册双张量 → WRITE 张量→FAR 池 slot → READ FAR slot→张量2 → 校验 →
  NPUGraph 捕获/重放（注册先于捕获）→ unregister 后 copy 被 host 预检拒绝 →
  host-DRAM 地址 register 被拒。

## 拓扑与角色

```
FAR 节点（常驻守护，复用 09）              NEAR 节点（客户端）
┌────────────────────────────┐           ┌─────────────────────────────────────┐
│ contributor 贡献 DRAM slot │           │ torch 张量 src/dst（HBM，register） │
│ （executor 自动以          │◄──────────│ NPUGraph: device_copy               │
│  AI_CORE_INITIATE 加入池） │ RDMA WRITE │   ├─ 用户 MR 表查 lkey/地址         │
│                            │  (AICore   │ 捕获 1 次 → replay K 次 → 读回校验  │
│                            │   自发)    │                                     │
└────────────────────────────┘           └─────────────────────────────────────┘
```

## 使用能力
- `handle.register(addr, size)` / `handle.unregister(addr)`：用户 HBM 内存注册/注销，
  设备侧经 64K user context 区的用户 MR 表（v2：entry 含 regAddress）取 lkey 与
  device-dma 基址；注册/注销必须在 NPU Graph 捕获之外、注销前张量不得释放
- 设备侧 `post_send` 对称双分支：池内端点走池 MR（regAddress 换算），池外端点走用户
  MR（`regAddress + (localAddr - addr)`，HBM 下恒等）
- `device_copy` 端点组合：本地用户内存 × 对端池 slot（本用例即此），host 预检
  （classifyEnd）已支持 USER 端点判定

## 快速开始（跨节点手工分别启动）

```bash
# 1) FAR 节点启动守护（与 09 相同），等到 "... memory contributors serving (...)"
python3 memfabric_daemon.py --store tcp://<far_ip>:8587 --devs 0,1

# 2) NEAR 节点跑客户端：建池 → 注册张量 → warmup → 捕获 → 重放 → 校验 → 负例
python3 memfabric_client.py --store tcp://<far_ip>:8587 --dev 1

# 3) 停止守护
kill -TERM <daemon_pid>
```

## 生命周期
- 客户端 create → extend（本地 + 远端 DRAM slot，FAR 侧为落点）→ 注册双张量 →
  warmup → 捕获 → 重放 → 读回校验 → 负例（注销后 copy、host 地址注册）→ destroy
  全链路自清理，退出无需通知守护
- 守护生命周期与 09 完全一致
- 客户端每次运行以 `"w"` 截断重写 `log/near_dev{N}.log`

## 参数

**memfabric_daemon.py**：与 09 完全相同（`--store/--devs/--world/--rpc-port-base/--run-dir`）。

**memfabric_client.py**

| 参数 | 默认 | 说明 |
|---|---|---|
| `--store` | 必填 | 守护进程的 store url |
| `--dev` | 必填 | 客户端使用的 NPU id |
| `--size` | 1M | 每张量/每次单边拷贝的字节数（K/M/G 后缀，须 4 字节对齐） |
| `--replays` | 8 | 捕获后重放次数（计时段） |
| `--block-size` | 64M | 两侧各提交的 DRAM slot 字节数（FAR 落点，须 ≥ size） |
| `--max-pool-size` | 4G | 池 DRAM 窗口（**必须 GB 对齐**） |
| `--world` | 512 | 同守护 |
| `--rpc-port-base` | 11100 | 同守护 |
| `--run-dir` | ./log | 客户端日志目录（`near_dev{dev}.log`） |

## 必要条件
- 在 09 的必要条件之上：
- **两端版本一致**：用户 MR 表 v2（kernel 与 host 同步升级，旧内核拒识新表）
- 张量必须由 torch NPU 分配（HBM）；host 内存（含 pinned）属于 P2 范围，注册即被拒
- 注册必须在捕获前（本用例已内置顺序）；注销后不得再以该地址发起 device_copy

## 验收标准
- 客户端关键行依次出现：
  1. `device-scheduled pool created (DEVICE_RDMA | DEVICE_SCHEDULE)`
  2. `host-DRAM register rejected as expected (P1: NPU HBM only)`
  3. `both tensors registered (user MR table published to the meta window)`
  4. `warmup round-trip OK (tensor -> FAR slot -> tensor2, kernel library loaded)`
  5. `graph captured: 1 x <size> byte device-scheduled WRITE from the user tensor, no host interaction inside`
  6. `<replays> replays done: ... GB/s device-scheduled`
  7. `verify OK: FAR rank <R> slot matches the tensor pattern`
  8. `post-unregister copy rejected as expected (host precheck)`
  9. `[client] user-registered HBM endpoints under NPU graph finished cleanly`，exit 0
- 守护侧正常常驻、停机 `all contributors stopped cleanly`

## 判读
- replay 吞吐语义与 09 相同（验证可捕获性与正确性，非极限带宽）
- warmup 即失败：看 `register` 返回与 C 层日志（非 HBM 拒绝/表发布失败）
- verify 失败但 warmup 成功：优先怀疑 replay 期间链路/对端异常，`log/far_dev{N}.log`
  找 CQE 状态打印

## 排障（精简）
| 症状 | 处置 |
|---|---|
| register 报 `reject_non_hbm` | 传入地址不在平台 HBM 段：确认张量由 `device="npu"` 分配（P1 仅支持 HBM；host DRAM 属 P2） |
| device_copy 报 `copy endpoint falls neither into the pool device window nor a registered user region` | 张量未 register 或已 unregister（本用例负例 2 即验证该路径） |
| 捕获阶段报错/崩溃 | 注册是否在捕获外完成（本用例已内置）；warmup 是否在捕获外执行过 |
| 用户 MR 表 version 不符 | 一端为旧包：两端同步重装（表 v2 与旧内核互不识别） |
| 其余与 09 相同 | 参照 09_near_device_scheduled_rdma 排障表 |
