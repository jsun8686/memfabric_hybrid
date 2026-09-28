# 13_user_dram_registered_memory

## 场景

在 10_user_registered_memory（NPU HBM 张量注册为设备调度拷贝端点）的基础上，把本地端点换成**主机 DRAM 缓冲**：客户端用匿名 `mmap` 分配 4K 对齐的 host 内存，经 `handle.register(addr, size)` 注册后，作为 `device_copy` 的本地端点，与 FAR 池槽做单向 WRITE / READ、NPU 图捕获与重放，并在主机侧做 pattern 校验。

与例 10 的关键差异：host-DRAM 注册的 MR 经 `HalHostRegister` 映射，携带与注册地址不同的设备 DMA 基址（IOVA）。主机把该 IOVA 填入用户 MR 表 v2 的 `regAddress` 字段（**条目内字节偏移 +24**，`uint64_t` 下标 3），AICore RDMA 内核按 `regAddress + (localAddr - addr)` 推导 SGE 地址，在 MR 的 lkey 下直接寻址主机内存——内核侧零改动（v2 表格式与例 10 完全一致）。

前置约束：host-DRAM 端点要求**地址与大小均 4K 对齐**（IOVA 映射的页粒度），不对齐的注册在入口被拒（`SM_INVALID_PARAM`）；例 10 中"host-DRAM 注册被拒"的负例在本例反转为正例 + 对齐负例。

**56-bit GVA 契约**：池槽 GVA 仅作 device_copy 端点，**禁止 CPU 解引用**（`--enable-56bits-gva` 下 GVA 窗位于 2^55 以上、无 CPU 映射）；本例的 CPU 侧数据面全部经注册用户缓冲。

**根因记录（历史）**：曾出现"用户缓冲作为设备侧端点时数据全零"——原探针矩阵（A/B/B4/B5/B6/B6b/B7/B8/C2，按页状态/数据驻留/页型/VA 高度/窗口落位五因素切割）定位为**用户 MR 表 `regAddress` 字段序列化偏移错位**：发布侧误写条目内 +32（`uint64_t` 下标 4），内核结构体 `SmemRallocUserMrEntry` 从 +24 读取 → 内核恒读 0 → fallback 用裸用户 VA 作 SGE，而 MR 注册在 IOVA 窗口上 → NIC 全零。例 10 的 HBM 路径因恒等映射（regAddress==addr，fallback 地址恰好正确）**掩蔽**了该缺陷。修复：发布侧改写下标 3（+24）。附带实测结论：`HOST_MEM_MAP_DEV(0)` 在本驱动（C23/23.x）上 `HalHostRegister` 直接失败（ret=65534，需驱动 ≥24.1，LMCache#91 同款），vendored 常量保持 3（DEV_PCIE_TH）；`device_copy` 端点分类改为**显式用户注册优先于 GVA 窗口归属**（classifyEnd 顺序交换）。探针矩阵修复后全绿，已从代码中移除（保留本记录）。

## 拓扑与角色

- FAR 节点（1 台）：`memfabric_daemon.py` 贡献 NPU 卡内存，内置 store（`tcp://<far_ip>:8588`）。
- NEAR 节点（1 台）：`memfabric_client.py`，建 DEVICE_RDMA | DEVICE_SCHEDULE 池（DRAM 窗口 4G + 仅元数据的 HBM 窗口 1G），本地注册 host 缓冲，remote extend 取 FAR 落地槽。
- 数据面端口基值 10010（`set_nic`），控制面 RPC 端口基值 11105（与例 09/10/11 相同，同一时刻只跑一个示例即可复用）。

## 使用能力

- `handle.register(addr, size)`：注册 4K 对齐主机内存（本例主场景）或 NPU HBM（例 10 场景，行为不变）。
- `handle.device_copy(src, dst, size, stream)`：本地端点 = 已注册 host 缓冲、对端 = FAR 池槽的单边拷贝。
- NPU 图捕获/重放：host 端点地址固定（mmap），捕获后重放无主机交互。
- `handle.unregister(addr)`：注销后主机预检查拒绝后续拷贝（负例）。
- `--enable-56bits-gva`：56-bit GVA 模式建池（GVA 仅设备端点）。

## 快速开始（跨节点手工分别启动）

```bash
# 1) FAR 节点启动守护（与 11 相同），等到 "... memory contributors serving (...)"
python3 memfabric_daemon.py --store tcp://<far_ip>:8588 --devs 0,1

# 2) NEAR 节点跑客户端：建池 → 对齐负例 → 注册 host 缓冲 → 往返 → 捕获 → 重放 → 校验 → 注销负例
python3 memfabric_client.py --store tcp://<far_ip>:8588 --dev 0
#    56-bit GVA 变体：加 --enable-56bits-gva

# 3) 停止守护
kill -TERM <daemon_pid>
```

## 生命周期

`create`（DEVICE_RDMA | DEVICE_SCHEDULE，可选 56-bit GVA）→ `extend_local_mem`（本地 DRAM 槽，保证实体 dramSegment 就绪）→ `extend_remote_mem`（FAR 落地槽）→ 对齐负例 ×2 → src/dst 注册 → 往返（host buffer → FAR 槽 → host buffer2）→ `NPUGraph` 捕获 WRITE → `replay` ×N → 默认流 READ 回读校验 → `unregister` ×2 + 预检查负例 → `destroy` → `uninitialize`。

## 参数

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--store` | 必填 | FAR 守护的 store url |
| `--dev` | 必填 | 客户端所在 NPU id |
| `--size` | 1M | 每缓冲/每次单边拷贝字节数，须 4K 对齐且 4 字节对齐 |
| `--replays` | 8 | 捕获后的图重放次数 |
| `--block-size` | 64M | 两侧 DRAM 槽字节数（FAR 落地带） |
| `--max-pool-size` | 4G | 池 DRAM 窗口，须 GB 对齐（VMM 段规则） |
| `--world` / `--rpc-port-base` | 512 / 11105 | 与守护一致 |
| `--enable-56bits-gva` | 关 | 56-bit GVA 建池（GVA 窗位于 2^55 以上，仅设备端点） |
| `--run-dir` | ./log | 日志目录 |

## 必要条件

- 两节点已安装同版本 `memfabric_hybrid`（含 v2 用户 MR 表 regAddress +24 序列化修复，双端同 commit）。
- FAR 侧池 DRAM 槽依赖大页（与例 07/11 相同的宿主机大页配置）；56-bit 模式下大页充足尤其重要（巨页不足时 conn 段会走 halMemAlloc 回退，该路径在 V3 符号必败）。
- NEAR 侧普通匿名内存即可（`mmap`），无需大页；主机缓冲地址由内核保证 4K 对齐。

## 验收标准

1. 客户端日志出现 `unaligned host-DRAM register rejected as expected (4K alignment rule)`。
2. `src/dst registered`，near 日志有对应 `RegisterMem ok` ×2（src/dst）。
3. `round-trip OK (host buffer -> FAR slot -> host buffer2)`。
4. `graph captured` 后 `N replays done`，吞吐量级与例 10 相当（同为单流单拷贝，量级参考即可）。
5. `verify OK: FAR rank <r> slot matches the host pattern`。
6. `post-unregister copy rejected as expected (host precheck)`。
7. 退出前 `mf.get_last_err_msg()` 为空，FAR 守护各 contributor `stopped cleanly`。

## 判读

- 注册层：near 日志 `RegisterMem ok: addr=0x... size=...`；宿主 `query memory key ok` 行应显示 `mrAddr` 等于注册地址、`regAddress` 为另一 IOVA 值（host-DRAM 特征，HBM 时两者相等）。
- 数据层：往返与 verify 两次独立校验，replay 后 verify 再次校验，覆盖"图重放期间数据未漂移"。
- 负例层：对齐负例在注册入口被拒（错误日志含 `reject_unaligned_dram`）；注销后拷贝在主机预检查被拒。
- 56-bit 层：`--enable-56bits-gva` 下 near 日志 `AllocReserveGva` 的 GVA 基址应落 [64P,128P)（0x4000000000000000 以上），全部校验同样通过。

## 排障（精简）

| 现象 | 首查 |
| --- | --- |
| 注册返回非 0，日志 `reject_unaligned_dram` | 地址或大小非 4K 对齐（torch pinned tensor、`ctypes.create_string_buffer` 均不保证，请用 `mmap`） |
| 往返/verify mismatch 且 host 端读到全零 | regAddress 序列化偏移错位的旧版本（核对双端 commit 含 `entry[3]`/+24 修复）或双端版本不一致；near 日志核对 `register MR result` 的 regAddress 应为独立 IOVA |
| `device_copy` 预检查拒绝 `srcType: 2 dstType: 2` | 端点分类：显式用户注册优先于 GVA 窗口归属（classifyEnd 顺序）；窗内地址须先 `register` 再作端点 |
| `query memory key failed` | 实体无 dramSegment：确认建池 `max_dram_size > 0` 且 `extend_local_mem` 已成功 |
| register 报表满 `user_mr_table_full` | 单进程用户 MR 上限 2040 条 |
| 池注册失败 `register host va failed, ret:65534` | `HOST_MEM_MAP_DEV` 常量被改为 0：本驱动（C23/23.x）flag=0 直接失败，保持 vendored 值 3（DEV_PCIE_TH） |
| 56-bit 下 CPU 直访槽地址段错误 | 契约：GVA 仅设备端点（见"56-bit GVA 契约"）；CPU 收发走注册用户缓冲 |
| 与其他示例端口冲突 | 同一时刻只跑一个示例，或统一改 `--rpc-port-base` 与 NIC_PORT_BASE |
