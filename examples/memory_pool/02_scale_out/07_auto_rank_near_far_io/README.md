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

## 启动步骤（跨节点手工分别启动）

```bash
# 1) 每个 FAR 节点各执行一次（store URL 指向第一个 FAR 节点 IP；常驻，Ctrl+C 或 shutdown 停止）
python3 07_auto_rank_near_far_io.py far  --store tcp://<far1_ip>:8587

# 2) 每个 NEAR 节点各执行一次（等待 FAR 就绪后自动跑完退出，exit 0 = 通过）
python3 07_auto_rank_near_far_io.py near --store tcp://<far1_ip>:8587 \
       --workers 4 --sizes 64K,256K,1M,4M,16M --mb-per-size 256

# 3) 停止 FAR 守护（或直接 Ctrl+C）
touch log/shutdown.json
```

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
