# 16 NVMe SSD 远程池（NVMe-oF 第三介质）

以 NVMe-oF 远程 SSD 作为第三种介质（DRAM / HBM 之外）接入 memfabric_hybrid：GVA 编址与
`copy_data` 统一 API 不变，SPDK 全部内化，对用户透明。本示例为**纯 NVMe 单介质池**，
演示完整闭环：FAR 节点贡献 NVMe LBA extent，NEAR 客户端通过统一 API 做**字节粒度读写**
（含亚扇区读改写）与**读写带宽矩阵**。

```
        FAR 节点                                    NEAR 节点
   ┌─────────────────────────┐            ┌────────────────────────────┐
   │ SPDK nvmf_tgt (数据面)   │◄─ NVMe-oF ─│ client: dlopen SPDK initiator│
   │ memfabric_daemon (控制面)│◄─ ralloc ──│ ralloc pool (NVME_TCP) │
   │ setup_nvmf_tgt.sh 签发ns │    RPC     │ copy_data(本地 ⇄ NVMe GVA)  │
   └─────────────────────────┘            └────────────────────────────┘
```

数据路径：SSD → FAR NIC → NEAR NIC → NEAR pinned DRAM（staging）→（可选 SDMA）→ HBM。
写路径以 NVMe CQE 为完成语义（CQE 即落盘，易失池：target/ns 回收即作废）。

## 前置条件

| 项 | 说明 |
|---|---|
| SPDK | 源码树（`SPDK_ROOT`，推荐）或包安装；**须以 `./configure --with-shared` 构建**（默认只产 `.a` 静态库，NEAR 数据面 dlopen 需要 `.so`；共享形态同时依赖 `<spdk>/dpdk/build/lib` 下的 DPDK 库）。需要 `nvmf_tgt`、`rpc.py`、`build/lib` 下 `libspdk_nvme.so`/`libspdk_env_dpdk.so` |
| isa-l 共享库 | SPDK 默认把 isa-l **静态链入** target，dlopen 宿主拿不到其符号，需手动产共享版并部署（SPDK 重编后需重做）：`cd <spdk>/isa-l && ./autogen.sh && ./configure && make -j && cp .libs/libisal.so* <spdk>/build/lib/` |
| OpenSSL | 系统包提供 `libcrypto` 与 `libssl`（含 `EVP_MD_CTX_free`/`OPENSSL_init_ssl`，即 OpenSSL 1.1+/3.x） |
| memfabric_hybrid | python 包装载可用（`import memfabric_hybrid`） |
| nvmf target | `setup_nvmf_tgt.sh` 部署（见下）；容器内无 systemd 时脚本自动降级直拉（stale socket 自动探测清理） |

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

# 3) client（NEAR 角色）：纯 NVMe 池，协议由 op 位声明（默认 NVMe/TCP，无需任何传输库）
#    注意双库路径：spdk/build/lib（spdk .so）+ spdk/dpdk/build/lib（DPDK 依赖）
export LD_LIBRARY_PATH=/home/s00799406/spdk/dpdk/build/lib:/home/s00799406/spdk/build/lib:$LD_LIBRARY_PATH
python3 memfabric_client.py --store tcp://127.0.0.1:8588
#    双节点/跨容器：--nvme-transport rdma（RDMA target）
```

> 容器提示：无 systemd 时脚本 `nohup` 直拉 nvmf_tgt（日志 `/tmp/nvmf_tgt.log`，停止 `pkill nvmf_tgt`）；
> 用 SPDK 自带工具（identify/perf）时 `-r` 串必须带 `adrfam:IPv4`，否则报 `Unhandled ADRFAM 0`。

### 预期输出（client）

```
[client rank N] pure-NVMe pool created (NVMe/tcp, nvme=1G)
[client rank N] NVMe block from FAR rank M: gva=0x..., 67108864 bytes
[client] aligned 67108864-byte round-trip OK (head/tail blocks distinct)
[client] sub-sector RMW OK (10B magic @+100 merged, neighbours preserved)
[client] bandwidth matrix (4096/65536/1048576/16777216 bytes):
    size      write GB/s   read GB/s
      4096        x.xx       x.xx
     65536        x.xx       x.xx
   1048576        x.xx       x.xx
  16777216        x.xx       x.xx
```

sim（TCP loopback + malloc 盘）数值仅用于功能验证，一般远低于真盘；真盘参考目标为多盘聚合
20GB/s+（RDMA transport）。

## 双节点真盘变体

1. FAR 节点：`sudo bash ./setup_nvmf_tgt.sh <存储面IP>`（默认 RDMA transport；脚本内
   `bdev_nvme_attach_controller` 行按实际 BDF 修改，多盘用 raid0 聚合，见脚本注释）；
2. daemon 的 `MF_NVME_BASE_BDEV` 指向真盘 bdev；extent 粒度按业务块大小设置；
3. client 用 `--nvme-transport rdma` 声明 RDMA 协议位（FAR 侧校验 target 监听一致性），
   `LD_LIBRARY_PATH` 指向 SPDK 库；
4. 同一 target 服务多个 FAR rank 时，用 `MF_NVME_LBA_BASE` 给各 rank 划分不相交 LBA 区间。

## 池 op 位与协议校验

- NVMe 协议由池 op 位显式声明：`RallocDataOpType.NVME_TCP` / `NVME_RDMA`（互斥，同一算子类，
  位仅决定协议，与 host 面 `HOST_RDMA/HOST_TCP` 惯例对称）；
- create/JOIN_ALLOC 控制面把位传到 FAR：FAR 在 NVMe 段预留时查询 target 实际监听的
  transport（`nvmf_get_transports`），**不一致直接阻断**（协议配置错误早失败，而非首个 IO 失败）；
- 本示例为纯 NVMe 单介质池（`max_dram=0`）；若需 NVMe+DRAM 双介质池，DRAM 窗口另配一个
  host 面 op 位（同节点 `HOST_SHM`、跨节点 `HOST_RDMA`），冷层预取工作流见下节。

## 环境变量

完整列表见 `doc/environment_variables.md`（NVMe 第三介质环境变量一节）。关键项：

| 变量 | 侧 | 说明 |
|---|---|---|
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
  冷数据工作流 = `copy(nvmeGva → DRAM 块)` 预取 + 现有 v3 gather（双介质池用法，
  15_hisparse_kv_gather 的冷层以此方式接入）；
- NVMe 协议位（`NVME_TCP`/`NVME_RDMA`）互斥，且与 `DEVICE_SCHEDULE` 互斥（host 发起专用）。

## 常见问题

| 现象 | 处理 |
|---|---|
| `Unhandled ADRFAM 0` | SPDK 工具 `-r` 串补 `adrfam:IPv4`；memfabric 数据面已内置修复 |
| 大块写被 target 拒绝 | 已按 MDTS 自动拆分；若仍现，检查 `MF_NVME_BUF_MB` 与 target `max_io_size` |
| create 报协议不一致（NOT_SUPPORTED） | 池 op 位声明的协议与 target 监听不符：sim target 是 TCP，需 `NVME_TCP` 位/`--nvme-transport tcp` |
| extend_remote_mem(NVME) 一直失败 | daemon 侧 `MF_NVME_*` 是否齐全、target 是否可达（`rpc.py nvmf_get_subsystems`）、粒度是否大于盘容量 |
| client 首次 IO 报库加载失败 | `LD_LIBRARY_PATH` 须含**双路径**（`<spdk>/build/lib` + `<spdk>/dpdk/build/lib`）；SPDK 须 `--with-shared` 构建（否则只有 `.a`） |
| dlopen 报 `undefined symbol: XXX` | SPDK 共享库的部分依赖未进 DT_NEEDED（原生程序靠链接期全家桶），加载器已内置预载（`RTLD_GLOBAL` 注入）：libcrypto → libisal → DPDK 全家 → libssl。符号对照：`EVP_MD_CTX_free`→libcrypto；`crc16_t10dif`→libisal（见前置条件）；`rte_*`→DPDK 目录扫描（默认扫 `LD_LIBRARY_PATH`，可用 `MF_NVME_DPDK_LIB_DIR` 显式指定）；`OPENSSL_init_ssl`→libssl。仍缺则按符号归属补系统库 |
| connect 报 `sock connection error` 但 strace 看不到 `connect()` 系统调用 | sock 实现（`libspdk_sock_posix.so`）未注册：其 constructor（`SPDK_NET_IMPL_REGISTER_DEFAULT`）只在库被加载时运行，且不在任何 DT_NEEDED 里——加载器已内置预载第 5 环，随库重编生效 |
| IO 前报 `alloc rmw scratch failed` | DPDK EAL 未初始化——`spdk_dma_malloc` 无内存子系统可用；加载器已内置 `rte_eal_init --in-memory`（**无需 hugepage 配置**），随库重编生效 |
| connect 阶段段错误（keyring/TLS 栈） | SPDK 24.09+ 的 `spdk_nvme_connect` 为三参 ABI，旧单参调用把寄存器垃圾当 opts（含 TLS key 指针）；已按 v25.05 对齐三参与 trid 布局（sizeof 604），随库重编生效 |
