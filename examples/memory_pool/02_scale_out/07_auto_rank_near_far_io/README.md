# 07_auto_rank_near_far_io

## 场景
多物理节点 scale-out IO 矩阵：auto-rank 动态编队，**store 主与常驻贡献端在 FAR 侧**，NEAR 侧多进程并发、多粒度对远端 HBM 块做 device RDMA 拷贝矩阵，测完即退（FAR 不受影响，可承接下一轮 NEAR）。

## 拓扑与角色

```
FAR 节点（常驻守护，每节点一条命令）          NEAR 节点（测完即退）
┌────────────────────────────────┐          ┌────────────────────────────────┐
│ 探测 LINK UP 的 NPU（hccn_tool）│          │ 探测 LINK UP 的 NPU            │
│ 每张 UP 卡 ── 一个 fardev 守护   │◄────────►│ W 个 nearworker 并发进程        │
│ store URL 指向 FAR 节点 ─        │ device   │ （各自 auto-rank + 独立 handle │
│ store 主/master 服务落 FAR 侧 ✓  │  RDMA    │  轮转分卡）                    │
└────────────────────────────────┘ 同交换机  │ 粒度扫描→round-trip→退出       │
                                            └────────────────────────────────┘
```

## 使用能力
- auto_ranking 动态编队（`ralloc.get_rank_id()` 获取分配结果，rank 顺序即加入顺序）
- store 服务端竞速（`start_store=True` + store URL 落在 FAR 节点 → store 主/master 在 FAR 侧）
- device RDMA 媒体（`DEVICE + SDMA|DEVICE_RDMA`，HBM-only 窗口；910B 上 DRAM 窗口带不了 SDMA 位）
- `extend_remote_mem` / `copy_data` / `wait` 多粒度矩阵 + 并发进程

## 设备选取规则
两端一致：仅 `hccn_tool -i N -link -g` 报 `link status: UP` 的 NPU 参与；
`--devs 0,3` 或环境变量 `MF_TEST_RDMA_DEVS` 可强制指定（被指定的卡若非 UP 直接报错退出）。

## 启动步骤（跨节点手工分别启动；**顺序是硬性要求**）

```bash
# 0) 硬性前置：旧会话两端全停（脚本会自检本节点残留并拒绝启动，但看不到对端节点）
ps -ef | grep 07_auto_rank | grep -v grep   # 两端各查一次，有则 kill 后再继续

# 1) 每个 FAR 节点各执行一次；等到终端打印 "[far] resident contributors: ..." 才算就绪
python3 07_auto_rank_near_far_io.py far  --store tcp://<far1_ip>:8587

# 2) 每个 NEAR 节点各执行一次（等待 FAR 就绪后自动跑完退出，exit 0 = 通过）
python3 07_auto_rank_near_far_io.py near --store tcp://<far1_ip>:8587 \
       --workers 4 --sizes 64K,256K,1M,4M,16M --mb-per-size 256

# 3) 停止 FAR 守护（或直接 Ctrl+C）；重启无需手动清 log/——脚本会自动清除上一轮的
#    shutdown/ready/done 标记文件（打印 "cleared stale markers"）
touch log/shutdown.json
```

> 时序违规的典型事故：NEAR 先于"旧 FAR 全停 + 新 FAR 就绪"启动，会加入尚存一息的旧会话；
> 旧 FAR 被杀后跨节点 QP 全部落空（`WaitQpReady timeout`），且新 FAR 守护会进入不可自愈的
> 坏状态（见排障·症状 C）。

## 参数
| 参数 | 默认 | 说明 |
|---|---|---|
| `--store` | 必填 | store url，**必须指向 FAR 节点**（store 主/master 落 FAR 的保证） |
| `--world` | 512 | 声明 world 容量（auto-rank 分配上限；实际成员动态加入，无需凑满） |
| `--devs` | 自动 | 强制 NPU id 列表（默认自动探测 LINK UP） |
| `--workers` | 4 | NEAR 并发 worker 进程数（上限即此值，轮转分卡） |
| `--sizes` | 64K,256K,1M,4M,16M | 粒度扫描列表（K/M/G 后缀） |
| `--mb-per-size` | 256 | 每粒度每 worker 单向流量（MB） |
| `--remote-mb` | 64 | 每 worker 申请的远端块大小（须 ≥ 最大粒度） |
| `--rpc-port-base` | 11100 | 控制面 rpc 端口基址（端口 = 基址 + rank_id）；整会话保持一致 |
| `MF_TEST_NIC_IP` | 自动 | 数据面 NIC IP 覆盖（同 03/04/05） |

## 必要条件
- 各节点 hccn_tool 可用（`hccn_tool -i N -link -g` 可查询链路状态）
- NEAR/FAR 选用卡的 device RDMA 链路 UP 且接在同一台交换机（跨节点可达）
- CANN + NPU 驱动正常（device 媒体需要）

## 验收标准
- FAR 侧：每个 LINK UP 卡一个 fardev 就绪（打印 `npu X -> rank Y`），常驻不退出
- NEAR 侧：W 个 worker 全部完成，打印每粒度 min/avg/max 吞吐表，末行
  `(W/W) 07_auto_rank_near_far_io: near IO matrix OK`，exit 0
- 每粒度含 untimed round-trip 内容校验（`torch.equal`），任一不匹配即失败
- 日志：`./log/far_dev*.log`、`./log/near_w*.log`；失败保留，成功删除（`MF_KEEP_LOGS=1` 保留）

## 判读
- 小粒度（64K/256K）吞吐受单块时延主导（`us_per_block` 列），大粒度逼近 device RDMA 带宽
- 多 worker 并发应摊满 NIC/链路带宽；若随 worker 数不增，检查交换机侧或同卡竞争

## 排障（会话残留）
控制面 rpc 端口 = `11100 + rank_id`（`smem_ralloc_def.h` 默认基址），**节点本地**——上一会话残留的同 rank 进程会蹲占完全相同的端口。两类典型症状与处置：

**症状 A：`far` 启动即报 `stale store suspected`**
上一会话的 store 进程仍在本机监听 8587。脚本已预检拦截（防静默接入旧 store 的脏 rank 计数与旧 master）。处置：
```bash
ss -lntp | grep 8587; ps -ef | grep 07_auto_rank | grep -v grep   # 找到残留
kill <残留PID>; rm -rf log/; python3 07_auto_rank_near_far_io.py far --store tcp://<far1_ip>:8587
```
干净重启的特征：首个 fardev 日志无 `address in use`、rank 从 0 起、出现 `master activated`。

**症状 B：near worker 报 `address in use for bind listen on <ip>:1110N` 后退出**
本节点有残留 worker 蹲占 `11100+rank` 端口（新会话分到相同 rank 即相撞；NEAR 父进程失败时会自动收割本次的兄弟 worker，但对**上一会话**的残留无能为力）。处置：
```bash
ss -lntp | grep -E '1110[0-9]|8587'; ps -ef | grep 07_auto_rank | grep -v grep   # 两个节点都查
kill <残留PID们>   # 然后重跑 near；若残留不便清理，可整会话换用 --rpc-port-base 11200 避开
```
注意换基址需 FAR 与 NEAR **同值**启动；rank 从 auto-rank 全局分配，端点随注册上报，与数据面无冲突。

**症状 C：near 日志出现 `WaitQpReady timeout` / `connect failed: -7`，FAR 侧刷 `-2003`（`GroupUpdate Assert joined_`）与 `-601` 键轮询**
跨会话互踩：NEAR 在"旧 FAR 全停 → 新 FAR 就绪"之间启动，先加入了垂死的旧会话；旧 FAR 进程被杀后，
已导入的 slice 指向死端点，跨节点 QP 30s 超时，join barrier 失败；新 FAR 守护随之进入**不可自愈**的
组状态（master 仍持续授予 placement，但每次 extend 都 -2003）。处置：两端全停（含 FAR 守护——
它不会自己恢复），按启动步骤从第 0 步重来。脚本已加同节点残留自检（`pgrep` 本测试进程，发现即拒绝
启动），但对端节点的残留仍需人工按第 0 步确认。

**症状 D（已修复，留作判读）：far 打印 resident 行后秒退、`all contributors exited cleanly`**
上一轮停止时 `touch log/shutdown.json` 留下的标记未清，新 fardev 一 ready 就看到它随即退出。现脚本在
父进程启动时自动清除陈旧 `shutdown.json`/`far_dev*_ready.json`/`near_w*_done.json`；若再见
`WARN: shutdown marker already present at startup` 说明标记在启动后才被落下（人为 touch）。
