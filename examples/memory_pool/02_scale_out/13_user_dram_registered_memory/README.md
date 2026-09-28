# 13_user_dram_registered_memory

## 场景

在 10_user_registered_memory（NPU HBM 张量注册为设备调度拷贝端点）的基础上，把本地端点换成**主机 DRAM 缓冲**：客户端用匿名 `mmap` 分配 4K 对齐的 host 内存，经 `handle.register(addr, size)` 注册后，作为 `device_copy` 的本地端点，与 FAR 池槽做单向 WRITE / READ、NPU 图捕获与重放，并在主机侧做 pattern 校验。

与例 10 的关键差异：host-DRAM 注册的 MR 经 `HalHostRegister` 映射，携带与注册地址不同的设备 DMA 基址（IOVA）。主机把该 IOVA 填入用户 MR 表 v2 的 `regAddress` 字段（**条目内字节偏移 +24**，`uint64_t` 下标 3），AICore RDMA 内核按 `regAddress + (localAddr - addr)` 推导 SGE 地址，在 MR 的 lkey 下直接寻址主机内存——内核侧零改动（v2 表格式与例 10 完全一致）。

前置约束：host-DRAM 端点要求**地址与大小均 4K 对齐**（IOVA 映射的页粒度），不对齐的注册在入口被拒（`SM_INVALID_PARAM`）；例 10 中"host-DRAM 注册被拒"的负例在本例反转为正例 + 对齐负例。

**根因记录（已修复，本例即探针矩阵定位过程）**：曾出现"用户缓冲作为设备侧端点时数据全零"——八探针（页型/注册时页状态/DMA 时数据驻留/VA 高度/窗口内外）**同质失败**，据此排除全部 VMA 属性假设，定位为**用户 MR 表 `regAddress` 字段序列化偏移错位**：发布侧误写条目内 +32（`uint64_t` 下标 4），内核结构体 `SmemRallocUserMrEntry` 从 +24 读取 → 内核恒读 0 → fallback 用裸用户 VA 作 SGE，而 MR 注册在 IOVA 窗口上 → NIC 全零。例 10 的 HBM 路径因恒等映射（regAddress==addr，fallback 地址恰好正确）**掩蔽**了该缺陷——v2 表此前从未有过 regAddress≠addr 的正例。修复：发布侧改写下标 3（+24），修复后九探针+warmup+图重放+verify 全绿。附带两条实测结论：`HOST_MEM_MAP_DEV(0)` 在本驱动（C23/23.x）上 `HalHostRegister` 直接失败（ret=65534，需驱动 ≥24.1，LMCache#91 同款），vendored 常量保持 3（DEV_PCIE_TH）；`device_copy` 端点分类改为**显式用户注册优先于 GVA 窗口归属**（classifyEnd 顺序交换，窗内用户注册按 USER 分类，为将来窗口切分用户 DRAM 铺路）。

## 拓扑与角色

- FAR 节点（1 台）：`memfabric_daemon.py` 贡献 NPU 卡内存，内置 store（`tcp://<far_ip>:8588`）。
- NEAR 节点（1 台）：`memfabric_client.py`，建 DEVICE_RDMA | DEVICE_SCHEDULE 池（DRAM 窗口 4G + 仅元数据的 HBM 窗口 1G），本地注册 host 缓冲，remote extend 取 FAR 落地槽。
- 数据面端口基值 10010（`set_nic`），控制面 RPC 端口基值 11105（与例 09/10/11 相同，同一时刻只跑一个示例即可复用）。

## 使用能力

- `handle.register(addr, size)`：注册 4K 对齐主机内存（本例主场景）或 NPU HBM（例 10 场景，行为不变）。
- `handle.device_copy(src, dst, size, stream)`：本地端点 = 已注册 host 缓冲、对端 = FAR 池槽的单边拷贝。
- NPU 图捕获/重放：host 端点地址固定（mmap），捕获后重放无主机交互。
- `handle.unregister(addr)`：注销后主机预检查拒绝后续拷贝（负例）。

## 快速开始（跨节点手工分别启动）

```bash
# 1) FAR 节点启动守护（与 11 相同），等到 "... memory contributors serving (...)"
python3 memfabric_daemon.py --store tcp://<far_ip>:8588 --devs 0,1

# 2) NEAR 节点跑客户端：建池 → 对齐负例 → 注册 host 缓冲 → warmup → 捕获 → 重放 → 校验 → 注销负例
python3 memfabric_client.py --store tcp://<far_ip>:8588 --dev 0

# 3) 停止守护
kill -TERM <daemon_pid>
```

## 生命周期

`create`（DEVICE_RDMA | DEVICE_SCHEDULE）→ `extend_local_mem`（本地 DRAM 槽，保证实体 dramSegment 就绪）→ `extend_remote_mem`（FAR 落地槽）→ 对齐负例 ×2 → src/dst **未 fault 注册** → 填充+驱逐 → b6 **DRAM 驻留注册** + src 重弄脏 + 巨页缓冲注册 + B7 低 VA 缓冲注册 + B8 窗内缓冲（MAP_FIXED 至池 GVA 窗内）注册 → 探针 A/B/B4/B5/B6/B6b/B7/B8/C2 三阶段（设备段间夹 CPU 驱逐/重写）→ warmup 闭环双向 → `NPUGraph` 捕获 WRITE → `replay` ×N → 默认流 READ 回读校验 → `unregister` ×5 + 预检查负例 → `destroy` → `uninitialize`。

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
| `--run-dir` | ./log | 日志目录 |

## 必要条件

- 两节点已安装同版本 `memfabric_hybrid`（含本例依赖的 v2 用户 MR 表 regAddress 填充，双端同 commit）。
- FAR 侧池 DRAM 槽依赖大页（与例 07/11 相同的宿主机大页配置）。
- NEAR 侧普通匿名内存即可（`mmap`），无需大页；主机缓冲地址由内核保证 4K 对齐。

## 验收标准

1. 客户端日志出现 `unaligned host-DRAM register rejected as expected (4K alignment rule)`。
2. `src/dst registered unfaulted` 与 `b6 registered DRAM-resident; src re-dirtied`，near 日志有对应 `RegisterMem ok` ×6（src/dst/b6/巨页/低VA/窗内）。
3. 探针组八连（判读见下）：`[probe A] pool slot source, cache-dirty (baseline): OK`、`[probe B] ... registered unfaulted, dirty fill`、`[probe B4] ... after 512MB eviction`、`[probe B5] hugepage-backed ...`、`[probe B6] ... DRAM-resident at register`、`[probe B6b] ... dirty post-register rewrite`、`[probe B7] user buffer at low VA ...`、`[probe B8] user hugepage inside the pool GVA window ...`、`[probe C2] ... far seeded via pool (dst pre-evicted)`（失败时行尾附首 8 字 dump）。
4. `warmup round-trip OK (host buffer -> FAR slot -> host buffer2)`。
5. `graph captured` 后 `N replays done`，吞吐量级与例 10 相当（同为单流单拷贝，量级参考即可）。
6. `verify OK: FAR rank <r> slot matches the host pattern`。
7. `post-unregister copy rejected as expected (host precheck)`。
8. 退出前 `mf.get_last_err_msg()` 为空，FAR 守护各 contributor `stopped cleanly`。

## 判读

- 注册层：near 日志 `RegisterMem ok: addr=0x... size=...`；宿主 `query memory key ok` 行应显示 `mrAddr` 等于注册地址、`regAddress` 为另一 IOVA 值（host-DRAM 特征，HBM 时两者相等）。
- 探针层（五因素矩阵：注册时页状态 × DMA 时数据驻留 × 页型 × VA 高度 × 窗口落位；修复后基线=九连 OK，各探针作回归切割保留）：

| 探针 | 注册时页状态 | DMA 时数据 | 页型/落位 | 切割维度 |
|---|---|---|---|---|
| A | 池槽（已驻留） | cache 脏 | 巨页 | 基线：池表 regAddress 换算正确 |
| B | 未 fault | cache 脏 | 4K | 注册时页状态（未 fault） |
| B4 | 未 fault | DRAM（驱逐后） | 4K | DMA 时数据驻留（DRAM） |
| B5 | 注册后填充 | cache 脏 | 巨页 | 页粒度（巨页后备） |
| B6 | **fault+数据已驻留** | DRAM | 4K | 注册时页状态（已驻留） |
| B6b | 同 B6 缓冲 | 重写后 cache 脏 | 4K | 注册后改写一致性 |
| B7 | 注册后填充 | cache 脏 | 4K @低 VA（hint 3TB，<2^47） | VA 高度 |
| B8 | 注册后填充 | cache 脏 | 巨页 @池 GVA 窗内（MAP_FIXED 本地槽 +1TB） | 窗口落位（依赖 classifyEnd 用户优先分类） |
| C2 | dst 预驱逐干净 | — | 4K | user 作宿（写方向） |

  判读：**任一用户探针 FAIL 且 far/dst 全零** ⇒ 首查双端是否含 regAddress +24 修复（偏移错位时内核 fallback 裸 VA，症状恰为全零；双端版本不一致亦如此）；其次 near 日志 `register MR result` 的 regAddress 是否为独立 IOVA、`query memory key` 行 mrAddr/regAddress 是否正常；A 单独 FAIL ⇒ 池数据面/环境问题（与用户 MR 无关）。
- 数据层：warmup 与 verify 两次独立校验，replay 后 verify 再次校验，覆盖"图重放期间数据未漂移"。
- 负例层：对齐负例在注册入口被拒（错误日志含 `reject_unaligned_dram`）；注销后拷贝在主机预检查被拒。

## 排障（精简）

| 现象 | 首查 |
| --- | --- |
| 注册返回非 0，日志 `reject_unaligned_dram` | 地址或大小非 4K 对齐（torch pinned tensor、`ctypes.create_string_buffer` 均不保证，请用 `mmap`） |
| `[probe A] FAIL` | 池数据面/环境问题（与用户 MR 无关）：核对大页、QP 连接、双端版本 |
| 用户探针（B 系/B8/C2）FAIL 且 far/dst 全零 | regAddress 序列化偏移错位的旧版本（核对双端 commit 含 `entry[3]`/+24 修复）或双端版本不一致；near 日志核对 `register MR result` 的 regAddress 应为独立 IOVA |
| `device_copy` 预检查拒绝 `srcType: 2 dstType: 2` | 端点分类：显式用户注册优先于 GVA 窗口归属（classifyEnd 顺序）；窗内地址须先 `register` 再作端点 |
| warmup/verify mismatch | 双端 `memfabric_hybrid` 版本不一致（v2 表 regAddress 修复只在一端）；确认两节点同 commit |
| `query memory key failed` | 实体无 dramSegment：确认建池 `max_dram_size > 0` 且 `extend_local_mem` 已成功 |
| register 报表满 `user_mr_table_full` | 单进程用户 MR 上限 2040 条 |
| 池注册失败 `register host va failed, ret:65534` | `HOST_MEM_MAP_DEV` 常量被改为 0：本驱动（C23/23.x）flag=0 直接失败，保持 vendored 值 3（DEV_PCIE_TH） |
| 与其他示例端口冲突 | 同一时刻只跑一个示例，或统一改 `--rpc-port-base` 与 NIC_PORT_BASE |
