# 08_far_daemon_near_client

## 场景
FAR 端**常驻内存守护进程** + NEAR 端**一次性客户端**的标准部署范式（对外发布指导用例）：

- 守护进程：每 FAR 节点一条命令，`--devs` 指定贡献的 NPU、每卡一个 contributor 子进程
  （`multiprocessing spawn` 管理），store/master 落在 FAR 侧，跨任意多轮客户端常驻服务；
- 客户端：连 store → `extend_remote_mem` 申请远端 DRAM 块 → device RDMA 拷贝
  （写/读 + 内容校验 + 吞吐打印）→ 释放全部资源 → 退出（exit 0 = 通过），
  守护进程不受影响，可立即承接下一个客户端。

## 拓扑与角色

```
FAR 节点（常驻守护，每节点一条命令）             NEAR 节点（客户端，随起随走）
┌──────────────────────────────────┐          ┌──────────────────────────────────┐
│ --devs 指定的 NPU（每卡一个      │          │ --dev 指定 NPU                   │
│ contributor 子进程）             │◄────────►│ extend → copy 矩阵 → 释放 → 退出 │
│ store URL 指向本节点 ─            │ device   │ 可对同一守护反复执行             │
│ store 主/master 服务落 FAR 侧 ✓   │  RDMA    │                                  │
└──────────────────────────────────┘          └──────────────────────────────────┘
```

## 使用能力
- `multiprocessing spawn` 多卡贡献端管理（禁 fork：CANN/hccp 驱动线程不可被 fork）
- auto_ranking 动态编队 + `dynamic_world_size`（客户端随起随走，守护不感知退出）
- store 服务端竞速（`start_store=True` 且 store URL 落 FAR 节点 → store 主/master 在 FAR 侧）
- `extend_remote_mem` 申请远端块（FAR 放置由 master 负载均衡，含在途授予账）
- `handle.register` 用户 HBM 直达拷贝 + `copy_data` / `copy_data_batch`（`--batch`）
- 守护生命周期：SIGTERM/Ctrl+C 干净停机；父进程被 kill -9 时子进程自检父 pid 自行退出

## 快速开始（跨节点手工分别启动）

```bash
# 1) FAR 节点启动守护，等到打印 "... memory contributors serving (...)" 即就绪
python3 08_far_memory_daemon.py --store tcp://<far_ip>:8587 --devs 0,1

# 2) NEAR 节点跑客户端（守护 banner 会直接给出这条命令）；可反复执行
python3 08_near_memory_client.py --store tcp://<far_ip>:8587 --dev 1

# 3) 停止守护（或前端 Ctrl+C）
kill -TERM <daemon_pid>        # pgrep -f 08_far_memory_daemon 找 pid
```

## 生命周期
- 客户端 create → extend → copy → destroy 全链路自清理；退出无需通知守护
- 守护停止：SIGTERM / Ctrl+C → 子进程走 `ralloc.uninitialize` 干净路径，守护收割校验退出码
- 守护被 `kill -9`：子进程通过父 pid 探测（1s 周期）自行干净退出，不残留
- 每次守护启动以 `"w"` 截断重写 `log/far_dev{N}.log`

## 参数

**08_far_memory_daemon.py**

| 参数 | 默认 | 说明 |
|---|---|---|
| `--store` | 必填 | store url，**必须指向本 FAR 节点**（store 主/master 落 FAR 的保证） |
| `--world` | 512 | 声明 world 容量（auto-rank 分配上限；实际成员动态加入） |
| `--devs` | 必填 | 贡献的 NPU id 逗号列表（如 `0,1`），每卡一个 contributor；卡不可用则该子进程快速失败 |
| `--rpc-port-base` | 11100 | 控制面 rpc 端口基址（端口 = 基址 + rank_id）；**与客户端同值** |
| `--run-dir` | ./log | per-contributor 日志目录（`far_dev{N}.log`） |

**08_near_memory_client.py**

| 参数 | 默认 | 说明 |
|---|---|---|
| `--store` | 必填 | 守护进程的 store url |
| `--dev` | 必填 | 客户端使用的 NPU id |
| `--sizes` | 1M,8M | 拷贝粒度列表（K/M/G 后缀） |
| `--mb-per-size` | 64 | 每粒度单向流量（MB）；不足一个粒度时至少跑 1 块 |
| `--remote-mb` | 64 | 申请的远端块大小（须 ≥ 最大粒度） |
| `--batch` | 关 | 计时段改用每方向一次 `copy_data_batch`（整批一次提交 + 一次等待） |
| `--world` | 512 | 同守护 |
| `--rpc-port-base` | 11100 | 同守护 |

## 必要条件
- 指定的 NPU 卡 device RDMA 链路 UP 且两端接在同一台交换机（跨节点可达）
- CANN + NPU 驱动正常（device 媒体需要）；客户端另需 torch/torch_npu

## 验收标准
- 守护侧：banner 打印 contributor 数与 `npu X->rank Y` 映射；停止后打印
  `[daemon] all contributors stopped cleanly`
- 客户端侧：每粒度一行含 `[round-trip OK]`，末行 `(1/1) 08_near_memory_client: client OK`，exit 0
- **连跑两轮客户端**，第二轮仍成功（守护复用范式）
- 日志：`./log/far_dev*.log`（每次守护启动重写）；客户端日志直打 stdout

## 判读
- 单客户端吞吐为可信参考（每 far NIC 一流时 ≈ perftest 线速的 ~97%）；多个客户端共享
  同一 FAR NIC 时聚合受该 NIC 线速约束，过载场景的计时会偏高，引用聚合数字前先算 NIC 上限
- 放置轮转：`grep "placement granted" log/far_dev*.log` 看 load 与目标 rank 的变化

## 排障（精简）
| 症状 | 处置 |
|---|---|
| 守护起来后 rank 不从 0 起 / 日志无 `master activated` | 本机有残留 store/守护在跑：`pgrep -f 08_far_memory`、`ss -lntp \| grep 8587`，kill 后重启 |
| 客户端报 `address in use ... 1110N` 后退出 | 残留进程蹲控制面 rpc 端口：`ss -lntp \| grep 111`，kill 残留，或整会话两端同换 `--rpc-port-base` |
| 客户端 `WaitQpReady timeout` / FAR 刷 `-2003` | 跨会话互踩（客户端加入了垂死的旧会话）：两端全停后按快速开始重来 |
| 守护 ready 超时 / `contributor died before ready` | 看 `log/far_dev{N}.log` 中 `ralloc.initialize` 的具体报错 |
