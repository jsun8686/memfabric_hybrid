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

## 跨机 RDMA（模拟盘）

单机 TCP sim 之外，还可用**真实 RDMA 网络背书模拟盘**跨机验证（FAR/NEAR 两节点）：

**拓扑**（示例 IP，按实际替换）：

| 角色 | 节点 | RDMA 口（数据面 4420） | 管理口（store/group RPC） |
|---|---|---|---|
| FAR（target+daemon） | 33 | 192.168.200.10（mlx5） | 10.90.22.33 |
| NEAR（client） | 35 | 192.168.200.11（mlx5） | 10.90.22.35 |

**前置**（两节点）：`/dev/infiniband/uverbs*` 容器内可访问；rdma-core（`libibverbs.so.1`/`librdmacm.so.1`）；RoCE 口互通（ping）。仅 FAR 侧：SPDK 须以 `./configure --with-shared --with-rdma` **重编**（增量 make 不感知 configure 变化，需 `make clean` 后全量）+ isa-l 共享库部署（见前置条件）；hugepages ≥2048×2M。NEAR 侧：从 FAR 同步 `spdk/build/lib` 与 `spdk/dpdk/build/lib` 至同路径 + 安装同版 whl。

**命令序列**：

```bash
# FAR(33)：模拟盘 + RDMA listener（traddr 用本机 RoCE IP）
sudo SPDK_ROOT=/home/s00799406/spdk bash ./setup_nvmf_tgt.sh 192.168.200.10 --sim --rdma
# daemon：MF_NVME_TRADDR=192.168.200.10（其余 env 同单机 sim），--store tcp://10.90.22.33:8588

# NEAR(35)：双库路径 + RDMA 协议位
export LD_LIBRARY_PATH=/home/s00799406/spdk/dpdk/build/lib:/home/s00799406/spdk/build/lib:$LD_LIBRARY_PATH
python3 memfabric_client.py --store tcp://10.90.22.33:8588 --nvme-transport rdma
```

控制面走管理口（store 8588 / rpc 端口段），数据面走 RDMA 口 4420——两网段均须互通。

## device 方向联调（LOCAL_DEVICE ↔ GLOBAL_NVME）

client 的 `copy_data` 方向按地址自动判定：默认模式的本地端点是 host mmap（LOCAL_HOST）；
`--device` 把本地端点换成 NPU HBM（torch_npu tensor 的 `data_ptr()` 即 DVA，落 NPU 虚拟
地址区间 → 自动判为 LOCAL_DEVICE），同一套验证/BW 流程即覆盖 device 两方向（本地 HBM 与
staging 间的最后一跳走 aclrtMemcpy/D2H/H2D）。**无需注册**——NVMe 算子只经
memcpy/aclrtMemcpy 中转，端点裸地址直接可用。

```bash
# 前置：NPU 节点（33/35 均可）+ python 环境有 torch_npu；--dev 已随 ralloc 初始化 device 上下文
python3 memfabric_client.py --store tcp://10.90.22.33:8588 --nvme-transport rdma --device
```

预期输出与 host 模式一致（round-trip / RMW / BW 矩阵），另有一行端点防呆日志
`device endpoints: w=0x... r=0x...`——确认地址落在 NPU 虚拟地址窗口内。BW 数值含
H2D/D2H 中转开销，与 host 模式分开解读。host 方向（LOCAL_HOST ↔ GLOBAL_NVME）已由
默认模式覆盖，两模式各跑一遍即 4 方向全覆盖。

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

## NUMA 亲和（可选，多 socket 机器）

数据 NIC 的 DMA 与业务线程/staging 内存跨 NUMA 时有实际损耗（本集群 NIC 在 node 6）：

- **NEAR 侧**：`--numa-node 6` 把 client 绑到 NIC 同 socket——NVMe 算子的 staging 池在
  copy 线程懒分配（first-touch）、qpair 也在该线程同步 poll，**一次绑核同时完成
  线程/内存/轮询三对齐**（须在进程启动早期生效，脚本内已置于 initialize 之前）；
- **device 方向全亲和**：再选 NIC 同 socket 的 NPU（本集群 c1/c2 侧卡 = `--dev 6` 或 7，
  可用 `npu-smi info` 的 PCI 列确认映射）；
- **FAR 侧**：setup 脚本在 RDMA 模式下自动按 listener IP 反查网卡 NUMA，把 tgt 的
  reactor 绑到该 node 的前 2 个 CPU（TCP loopback sim 保持 cpus 0-1）；
- 量化方法：`--numa-node 6`（绑对）vs `--numa-node 0`（绑错）两轮 BW 矩阵对比，差值即
  亲和收益。

## 零拷贝预注册（B3，host buffer 可选加速）

`pool.register(addr, size)` 对 NVMe 池的语义 = **零拷贝预注册**：把 buffer 的 2MB 对齐
内窗 `spdk_mem_register`（底层逐 mem map `ibv_reg_mr`），此后命中区间的 copy 由 NIC 直接
DMA 用户内存——stage-in/stage-out 两次 memcpy 消失（read 1M 实测瓶颈即在此）。

- **头尾碎片自然降级**：不足 2MB 对齐的头/尾（亚扇区 RMW 亦然）继续走 staging，混合执行，
  无需用户对齐 buffer；device（DVA）端点恒 staging；
- **显式生命周期**：buffer 须存活至 `pool.unregister(addr)`（与 host_rdma MR 注册同等
  契约）——释放后 VA 复用而注册仍活会 DMA 到错内存；
- **纯优化非依赖**：未注册/注册失败/`MF_NVME_ZEROCOPY=0`/符号缺失时自动回退 staging，
  正确性不受影响；A/B 对比用 `--no-zc`（staging 基线）；
- **RLIMIT_MEMLOCK**：注册 = 内核 pin 页（`ibv_reg_mr`），计入进程 memlock 配额——
  例 16 两个 60MB 内窗 ≈ 120MB，ssh/systemd 默认 64MB 会在第二个 register 报错。
  验证前 `ulimit -l unlimited`（或 systemd 单元 `LimitMEMLOCK=infinity`）；
  SPDK 对注册区间的要求：vaddr/len 均 2MB 对齐（向内收缩已处理）、不可重叠、
  须已映射；普通 4K 页匿名 mmap 即可，**不要求大页**（RDMA 数据面走 ibv_reg_mr，
  不经 vtophys/PRP——那是本地 PCIe NVMe 的路径）；
- 统一注册链：`pool.register` → ralloc → `hybm_register_local_memory`（与其他介质同一接口）
  → 纯 NVMe 池在 entity 层直落 NVMe transport 的 MR 域（`spdk_mem_register`），返回哨兵
  slice 句柄；IO 路径只做 ns 级查表，零注册开销（注册一次 = 整段一次 `ibv_reg_mr`，非逐 2MB）。

## 环境变量

完整列表见 `doc/environment_variables.md`（NVMe 第三介质环境变量一节）。关键项：

| 变量 | 侧 | 说明 |
|---|---|---|
| `MF_NVME_EXTENT_GRAN` | FAR | extent LBA 粒度（默认 1G，最小 2M）；**须 ≤ 模拟盘容量** |
| `MF_NVME_NQN`/`TRADDR`/`TRSVCID` | FAR | 本 target 对外身份，随块导出给 initiator |
| `MF_NVME_RPC_SOCK` | FAR | SPDK JSON-RPC socket（默认 `/var/tmp/spdk.sock`） |
| `MF_NVME_BASE_BDEV` | FAR | 池底座 bdev（真盘 `Nvme0n1` / 模拟 `Malloc0`） |
| `MF_NVME_BUF_MB` | NEAR | staging 单段上限（默认 4M）；实际段 = min(MDTS, BUF_MB, 尺寸) |
| `MF_NVME_QDEPTH` | NEAR | 窗口深度：单窗在飞命令数（默认 16，clamp 1..128） |
| `MF_NVME_QPAIRS` | NEAR | 每控制器 qpair 槽位数 = 并发窗口上限（默认 4，无全局 IO 锁） |
| `MF_NVME_BUF_SLOTS` | NEAR | staging 池深（默认 = QDEPTH） |
| `MF_NVME_ZEROCOPY` | NEAR | 零拷贝总开关（默认 1；0 = 全量回退 staging） |
| `MF_NVME_IOVA_MODE` | NEAR | EAL IOVA 模式（默认 va——用户内存零拷贝必需；auto = DPDK 自选，逃生口） |

> 数据面生命周期（性能优化后）：SPDK 库+EAL 在**池 create 时**预载（失败即报错）；
> ctrlr 连接与 qpair 在 **extent 导入时**预热（失败即 JOIN 报错并回滚）；首 IO 零建立开销。

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
| connect 报 `sock connection error` 但 strace 看不到 `connect()` 系统调用 | sock 实现（`libspdk_sock_posix.so`）未注册：其 constructor（`SPDK_NET_IMPL_REGISTER_DEFAULT`）只在库被加载时运行，且不在任何 DT_NEEDED 里。加载器已内置第 5 环预载，且**必须在 env 库之后**加载——`libspdk_util` 引用的 `spdk_realloc`（env 层符号）在 env 库进全局表前无法解析；预载失败会打 WARN（含 dlerror），随库重编生效 |
| connect 约 10 秒后断链（`CQ transport error -6`，target 日志 `keep alive timeout`） | SPDK 默认 KAT=10s 要求宿主**周期轮询 admin qpair** 发送 Keep Alive，本库按需轮询 io qpair 不满足——connect 已显式 `keep_alive_timeout_ms=0` 禁用 target 端监控（opts_size 前缀 ABI），空闲池不再断链 |
| IO 前报 `alloc rmw scratch failed` | DPDK EAL 未初始化——`spdk_dma_malloc` 无内存子系统可用；加载器已内置 `rte_eal_init --in-memory`（**无需 hugepage 配置**），随库重编生效 |
| connect 阶段段错误（keyring/TLS 栈） | SPDK 24.09+ 的 `spdk_nvme_connect` 为三参 ABI，旧单参调用把寄存器垃圾当 opts（含 TLS key 指针）；已按 v25.05 对齐三参与 trid 布局（sizeof 604），随库重编生效 |
