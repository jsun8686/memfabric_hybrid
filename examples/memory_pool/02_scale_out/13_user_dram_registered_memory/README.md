# 13_user_dram_registered_memory

## 场景

在 10_user_registered_memory（NPU HBM 张量注册为设备调度拷贝端点）的基础上，把本地端点换成**主机 DRAM 缓冲**：客户端用匿名 `mmap` 分配 4K 对齐的 host 内存，经 `handle.register(addr, size)` 注册后，作为 `device_copy` 的本地端点，与 FAR 池槽做单向 WRITE / READ、NPU 图捕获与重放，并在主机侧做 pattern 校验。

与例 10 的关键差异：host-DRAM 注册的 MR 经 `HalHostRegister` 映射，携带与注册地址不同的设备 DMA 基址（IOVA）。主机把该 IOVA 填入用户 MR 表 v2 的 `regAddress` 字段（`entry[4]`），AICore RDMA 内核按 `regAddress + (localAddr - addr)` 推导 SGE 地址，在 MR 的 lkey 下直接寻址主机内存——内核侧零改动（v2 表格式与例 10 完全一致）。

前置约束：host-DRAM 端点要求**地址与大小均 4K 对齐**（IOVA 映射的页粒度），不对齐的注册在入口被拒（`SM_INVALID_PARAM`）；例 10 中"host-DRAM 注册被拒"的负例在本例反转为正例 + 对齐负例。

**数据面行为（调查中）**：4K 匿名页用户缓冲作为设备侧**读源**时 NIC 读到全零——**与写入时序无关**（注册前/注册后两轮实证，探针 B）；池内巨页槽同链路正常（探针 A：同 flag、同 `HalHostRegister`、同 lkey/SGE 数学）。根因由探针切割：**B4**（512MB 缓存驱逐迫使 pattern 落 DRAM 后再写）OK ⇒ user-iova 读非一致（仅 DRAM 数据可见）；**B5**（巨页后备缓冲）OK ⇒ 页粒度/VMA 敏感，workaround=巨页/THP 缓冲；两者皆败 ⇒ 注册翻译钉错页。定案后本节更新；vendor 提单随附探针日志与 `register MR result`/`query memory key` 行。

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

`create`（DEVICE_RDMA | DEVICE_SCHEDULE）→ `extend_local_mem`（本地 DRAM 槽，保证实体 dramSegment 就绪）→ `extend_remote_mem`（FAR 落地槽）→ 对齐负例 ×2 → `register(src/dst)` + 巨页缓冲注册 → **注册后填充 pattern** → 探针 A/B/B4/B5/C2（池内闭环基线、4K 匿名作源、驱逐后重发、巨页作源、user 作宿）→ warmup 闭环双向 → `NPUGraph` 捕获 WRITE → `replay` ×N → 默认流 READ 回读校验 → `unregister` ×3 + 预检查负例 → `destroy` → `uninitialize`。

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
2. `both host buffers registered (user MR table v2: regAddress = HalHostRegister iova)`，且 near 日志有两条 `RegisterMem ok`。
3. 探针组（判读见下）：`[probe A] pool->pool round-trip ...: OK`、`[probe B] user 4K-anon as WRITE source ...`、`[probe B4] ... after 512MB cache eviction ...`、`[probe B5] hugepage-backed ...`、`[probe C2] ... READ sink ...`（失败时行尾附首 8 字 dump）。
4. `pattern filled after register (fixed protocol; write timing proven irrelevant)` 与 `warmup round-trip OK (host buffer -> FAR slot -> host buffer2)`。
5. `graph captured` 后 `N replays done`，吞吐量级与例 10 相当（同为单流单拷贝，量级参考即可）。
6. `verify OK: FAR rank <r> slot matches the host pattern`。
7. `post-unregister copy rejected as expected (host precheck)`。
8. 退出前 `mf.get_last_err_msg()` 为空，FAR 守护各 contributor `stopped cleanly`。

## 判读

- 注册层：near 日志 `RegisterMem ok: addr=0x... size=...`；宿主 `query memory key ok` 行应显示 `mrAddr` 等于注册地址、`regAddress` 为另一 IOVA 值（host-DRAM 特征，HBM 时两者相等）。
- 探针层（判读树）：A=池→池闭环基线（含 NIC 写池页的 CPU 可见性）；B=4K 匿名 user 作 WRITE 源；B4=同源驱逐缓存后重发（pattern 落 DRAM）；B5=巨页后备缓冲（MAP_HUGETLB，THP 备选，日志注明实际方式与 2M 对齐性）作源；C2=user 作 READ 宿（far 由池→池 WRITE 灌入 pattern，自足不依赖 B）。**B4 OK ⇒ user-iova 读非一致；B4 FAIL + B5 OK ⇒ 页粒度敏感；B/C2 全零且 B4/B5 亦败 ⇒ 翻译钉错页**；垃圾值=写错落点。
- 数据面行为（调查中）：4K 匿名读源全零与写入时序无关（B 两轮实证）；本节结论随 B4/B5 判定更新，vendor 提单随附探针日志。
- 数据层：warmup 与 verify 两次独立校验，replay 后 verify 再次校验，覆盖"图重放期间数据未漂移"。
- 负例层：对齐负例在注册入口被拒（错误日志含 `reject_unaligned_dram`）；注销后拷贝在主机预检查被拒。

## 排障（精简）

| 现象 | 首查 |
| --- | --- |
| 注册返回非 0，日志 `reject_unaligned_dram` | 地址或大小非 4K 对齐（torch pinned tensor、`ctypes.create_string_buffer` 均不保证，请用 `mmap`） |
| `[probe A] FAIL` | 池数据面/环境问题（与 user MR 无关）：核对大页、QP 连接、双端版本 |
| `[probe B] FAIL`（far=[0,0,...] 全零） | 已知现象（4K 匿名读源全零，写入时序无关）；按 B4/B5 行判根因 |
| `[probe B4] OK` | user-iova 读非一致（仅 DRAM 数据可见）：vendor 提单；短期可改用巨页缓冲（B5 方案） |
| `[probe B5] OK`（B4 FAIL） | 页粒度/VMA 敏感：巨页/THP 缓冲为 workaround；4K 匿名路径 vendor 提单 |
| `[probe B4] [probe B5] 均 FAIL` | 注册翻译钉错页：上报 `register MR result`、`query memory key` 行与探针日志 |
| `[probe C2] FAIL`（dst 空/垃圾） | user 作宿失效：A 正常时优先怀疑落点翻译；上报留档 |
| warmup/verify mismatch | 双端 `memfabric_hybrid` 版本不一致（v2 表 regAddress 未填充）；确认两节点同 commit |
| `query memory key failed` | 实体无 dramSegment：确认建池 `max_dram_size > 0` 且 `extend_local_mem` 已成功 |
| register 报表满 `user_mr_table_full` | 单进程用户 MR 上限 2040 条 |
| 与其他示例端口冲突 | 同一时刻只跑一个示例，或统一改 `--rpc-port-base` 与 NIC_PORT_BASE |
