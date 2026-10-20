# 16 NVMe SSD 远程池（NVMe-oF 第三介质）

以 NVMe-oF 远程 SSD 作为第三种介质（DRAM / HBM 之外）接入 memfabric_hybrid：GVA 编址与
`copy_data` 统一 API 不变，SPDK 全部内化，对用户透明。本示例演示完整闭环：FAR 节点贡献
NVMe LBA extent，NEAR 客户端通过统一 API 做**字节粒度读写**（含亚扇区读改写）、**读写带宽矩阵**
与 **ex15 冷层预取工作流**。

```
        FAR 节点                                    NEAR 节点
  ┌─────────────────────────┐            ┌────────────────────────────┐
  │ SPDK nvmf_tgt (数据面)   │◄─ NVMe-oF ─│ client: dlopen SPDK initiator│
  │ memfabric_daemon (控制面)│◄─ ralloc ──│ ralloc pool (HOST_RDMA|NVME)│
  │ setup_nvmf_tgt.sh 签发ns │    RPC     │ copy_data(本地 ⇄ NVMe GVA)  │
  └─────────────────────────┘            └────────────────────────────┘
```

数据路径：SSD → FAR NIC → NEAR NIC → NEAR pinned DRAM（staging）→（可选 SDMA）→ HBM。
写路径以 NVMe CQE 为完成语义（CQE 即落盘，易失池：target/ns 回收即作废）。

## 前置条件

| 项 | 说明 |
|---|---|
| SPDK | 源码树（`SPDK_ROOT`，推荐）或包安装；仅需 `nvmf_tgt`、`rpc.py`、`build/lib` 下 `libspdk_nvme.so`/`libspdk_env_dpdk.so` |
| memfabric_hybrid | python 包装载可用（`import memfabric_hybrid`） |
| nvmf target | `setup_nvmf_tgt.sh` 部署（见下）；容器内无 systemd 时脚本自动降级直拉 |

## 快速开始（单机无盘模拟，容器友好）

```bash
# 1) target：malloc 内存盘 + NVMe/TCP loopback（sudo；SPDK 源码树路径按需修改）
cd examples/memory_pool/02_scale_out/16_nvme_ssd_pool
sudo SPDK_ROOT=/home/s00799406/spdk bash ./setup_nvmf_tgt.sh 127.0.0.1 --sim

# 2) daemon（FAR 角色）：按脚本尾部输出的 daemon env 设置环境
export MF_NVME_NQN=nqn.2016-06.io.spdk:memfabric
export MF_NVME_TRADDR=127.0.0.1
export MF_NVME_BASE_BDEV=Malloc0
export MF_NVME_RPC_SOCK=/var/tmp/spdk.sock
export MF_NVME_EXTENT_GRAN=64M        # 重要：默认 1G 粒度下 1G 模拟盘只能容纳一个 extent
python3 memfabric_daemon.py --store tcp://127.0.0.1:8588

# 3) client（NEAR 角色）：TCP initiator + SPDK 库路径
export MF_NVME_TRSTRING=tcp
export LD_LIBRARY_PATH=/home/s00799406/spdk/build/lib:$LD_LIBRARY_PATH
python3 memfabric_client.py --store tcp://127.0.0.1:8588
```

> 容器提示：无 systemd 时脚本 `nohup` 直拉 nvmf_tgt（日志 `/tmp/nvmf_tgt.log`，停止 `pkill nvmf_tgt`）；
> 用 SPDK 自带工具（identify/perf）时 `-r` 串必须带 `adrfam:IPv4`，否则报 `Unhandled ADRFAM 0`。

### 预期输出（client）

```
[client rank N] pool created (HOST_RDMA | NVME, dram=4G, nvme=1G)
[client rank N] NVMe block from FAR rank M: gva=0x..., 67108864 bytes
[client] aligned 67108864-byte round-trip OK (head/tail blocks distinct)
[client] sub-sector RMW OK (10B magic @+100 merged, neighbours preserved)
[client] bandwidth matrix (4096/65536/1048576/16777216 bytes):
    size      write GB/s   read GB/s
      4096        x.xx       x.xx
     65536        x.xx       x.xx
   1048576        x.xx       x.xx
  16777216        x.xx       x.xx
[client] cold-tier prefetch OK (NVMe -> host -> DRAM block gva=0x..., v3 gather reads it unchanged)
```

sim（TCP loopback + malloc 盘）数值仅用于功能验证，一般远低于真盘；真盘参考目标为多盘聚合
20GB/s+（RDMA transport）。

## 双节点真盘变体

1. FAR 节点：`sudo bash ./setup_nvmf_tgt.sh <存储面IP>`（默认 RDMA transport；脚本内
   `bdev_nvme_attach_controller` 行按实际 BDF 修改，多盘用 raid0 聚合，见脚本注释）；
2. daemon 的 `MF_NVME_BASE_BDEV` 指向真盘 bdev；extent 粒度按业务块大小设置；
3. client 不设 `MF_NVME_TRSTRING`（默认 rdma），`LD_LIBRARY_PATH` 指向 SPDK 库；
4. 同一 target 服务多个 FAR rank 时，用 `MF_NVME_LBA_BASE` 给各 rank 划分不相交 LBA 区间。

## 环境变量

完整列表见 `doc/environment_variables.md`（NVMe 第三介质环境变量一节）。关键项：

| 变量 | 侧 | 说明 |
|---|---|---|
| `MF_NVME_TRSTRING` | NEAR | transport：`rdma`（默认）/`tcp`（模拟） |
| `MF_NVME_EXTENT_GRAN` | FAR | extent LBA 粒度（默认 1G，最小 2M）；**须 ≤ 模拟盘容量** |
| `MF_NVME_NQN`/`TRADDR`/`TRSVCID` | FAR | 本 target 对外身份，随块导出给 initiator |
| `MF_NVME_RPC_SOCK` | FAR | SPDK JSON-RPC socket（默认 `/var/tmp/spdk.sock`） |
| `MF_NVME_BASE_BDEV` | FAR | 池底座 bdev（真盘 `Nvme0n1` / 模拟 `Malloc0`） |
| `MF_NVME_BUF_MB`/`BUF_SLOTS`/`QPAIRS` | NEAR | staging 块大小（默认 4M）/池深（4）/每控制器 qpair 数（2） |

## 设计约束（摘要）

- **易失**：池/extent 回收即作废（ns 销毁断链），无持久化语义；
- **字节粒度**：任意对齐读写，亚扇区写走 read-modify-write（头/尾扇区合并）；
- **单命令上限**：按控制器 MDTS 自动拆分（nvmf TCP 默认 128KB），调用方无感；
- **AICore 侧**：`device_copy/v2/v3` 对 NVMe GVA 返回 `SM_NOT_SUPPORTED`（无 MR）——
  冷数据工作流 = copy 预取到 DRAM 块，再走现有 v3 gather（本示例第 5 步即该模式，
  也是 15_hisparse_kv_gather 的冷层接入方式）；
- NVMe op 位与 `DEVICE_SCHEDULE` 互斥（host 发起专用）。

## 常见问题

| 现象 | 处理 |
|---|---|
| `Unhandled ADRFAM 0` | SPDK 工具 `-r` 串补 `adrfam:IPv4`；memfabric 数据面已内置修复 |
| 大块写被 target 拒绝 | 已按 MDTS 自动拆分；若仍现，检查 `MF_NVME_BUF_MB` 与 target `max_io_size` |
| extend_remote_mem(NVME) 一直失败 | daemon 侧 `MF_NVME_*` 是否齐全、target 是否可达（`rpc.py nvmf_get_subsystems`）、粒度是否大于盘容量 |
| client 首次 IO 报库加载失败 | `LD_LIBRARY_PATH` 未含 SPDK `build/lib` |
