# 06_ha_master_switchover

## 场景
单机三进程 + etcd HA store：rank0=FAR（store 选举参与者，**独占先起保证首任 leader/master 确定**）、
rank1=NEAR（请求者，start_store=False）、rank2=FAR（store 选举参与者，幸存贡献者），固定 rank、world=3。
NEAR 取得远端块并往返校验后，父进程 SIGKILL rank0（首任 store leader + master 宿主）——lease 自然过期、
无干净交接，断言切主全链收敛与业务恢复。

## 目标（R12-A 切主联动）
kill rank0 后整条链路必须在恢复时限内完成：
etcd lease 5s 过期 → 幸存者健康检查发现 → 重选举（`Firing leader promotion handler`，胜者为 rank1/rank2
之一——选举对全部 HaConfigStore 实例开放，断言与胜者身份无关）→ `ActivateMasterOnPromotion` 重激活
（MASTER 键覆盖清理死 leader 遗留）→ FAR `master endpoint changed/refreshed` 重发现 → 新 master 候选表
重建 → NEAR `extend_remote_mem` 落到 rank2 并往返校验。

## 使用能力
`ralloc.initialize / create / extend_local_mem / extend_remote_mem / get_group_ranks / copy_data / destroy`。

## 必要条件
- 已安装同版本 memfabric_hybrid whl；单节点；数据面 IP 自动推导（`MF_TEST_NIC_IP` 环境变量 →
  hostname 解析 → UDP connect 取出口 IP → 127.0.0.1 兜底，集群裸主机名不可解析时自动走后两级）。
- **etcd 服务必须先启动**（单机即可）：
  ```bash
  etcd \
    --name=etcd-single \
    --data-dir=/var/lib/etcd \
    --listen-client-urls=http://0.0.0.0:2379 \
    --advertise-client-urls=http://0.0.0.0:2379 \
    --listen-peer-urls=http://0.0.0.0:2380 \
    --initial-cluster=etcd-single=http://0.0.0.0:2380 \
    --initial-cluster-state=new \
    --initial-cluster-token=etcd-single-cluster
  ```

## 验收标准
- 退出码 0，输出 `(1/1) 06_ha_master_switchover: failover OK`。
- rank1/rank2 日志**恰一个**含 `Firing leader promotion handler`（重选举胜者）；
  幸存者日志含 `master endpoint changed` 或 `master endpoint refreshed`；rank0 日志含 `Became leader`（基线）。
- 换点 `rank_id == 2`、组秩收敛 `[1, 2]`、往返数据一致；全部日志不含 `already watched for rank state`。
- 恢复时限 120s（lease 5s + 健康检查 4s + 选举退避 + FAR 重注册 ≤30s + 重试间隔）。
- 失败时运行目录保留（`mf_06_ha_switchover_*`），成功自动清理（`MF_KEEP_LOGS=1` 环境变量可让成功也保留 `log/`）；被杀进程残留由 OS 兜底。

## 运行
```bash
python3 06_ha_master_switchover.py                       # etcd://127.0.0.1:2379
python3 06_ha_master_switchover.py etcd://<ip>:2379
```
