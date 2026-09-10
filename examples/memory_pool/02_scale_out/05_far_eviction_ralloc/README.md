# 05_far_eviction_ralloc

## 场景
单机三进程故障注入：rank0=NEAR（store 宿主 + master + 请求者，04 拓扑压缩回单机）、rank1/rank2=FAR 贡献者，
固定 rank、world=3。NEAR 从某个 FAR 取得远端块后，父进程 SIGKILL 该 FAR（真崩溃，无清理、链路骤断），
断言 master 秒级剔除 + 换点重取。

## 目标（R12-B 死 FAR 剔除 + rank-watch 多 waiter + etcd 固定 rank 透传）
1. **rank-down 主信号**：被杀 FAR 的 store 链路断开 → master `OnRankDown` 擦候选表（秒级）；
2. **换点恢复**：NEAR 随后 `extend_remote_mem` 必须落到另一存活 FAR（竞窗内由同节点退避兜底）；
3. **组清理**：死成员从 `get_group_ranks()` 收缩（R13 LINK_DOWN 异步，retry 断言）；
4. **每链路多订阅**：NEAR 进程内 master 订阅 + 双池组引擎订阅共用一条 store 链路（多 waiter 修复的回归护栏，
   日志不得出现 `already watched for rank state`）；
5. **etcd 变体**（`--store etcd`）：固定 rank 经工厂透传在连接时登记——修复前 rank 不注册、rank-down 永不触发，
   本用例的 rank-down 日志断言必挂，即透传修复的硬校验。

## 使用能力
`ralloc.initialize / create / extend_local_mem / extend_remote_mem / get_group_ranks / copy_data / destroy`。

## 必要条件
- 已安装同版本 memfabric_hybrid whl；单节点；`MF_TEST_NIC_IP` 可覆盖数据面 IP（默认取节点主 IP）。
- etcd 变体需先启动 etcd（单机即可）：
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
- 退出码 0，输出 `(1/1) 05_far_eviction [tcp|etcd]: eviction + re-placement OK`。
- master（rank0）日志含 `candidate rank-down, rank: <victim> existed: 1`；换点后 `rank_id == 存活 FAR`；
  组秩收缩至 `[0, 存活 FAR]`。
- 全部日志不含 `already watched for rank state`；`placement reused failed rank` 为竞窗软指标（出现即报，不判失败）。
- 失败时运行目录保留（`mf_05_eviction_*`），成功自动清理；被杀子进程的 SIGKILL 残留（如共享内存段）由 OS 兜底。

## 运行
```bash
python3 05_far_eviction_ralloc.py            # tcp:// 变体（默认）
python3 05_far_eviction_ralloc.py etcd       # etcd://127.0.0.1:2379 变体
python3 05_far_eviction_ralloc.py etcd etcd://<ip>:2379
```
