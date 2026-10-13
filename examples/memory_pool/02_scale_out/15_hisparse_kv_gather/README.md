# 15_hisparse_kv_gather

## 场景
**HiSparse KV gather 的双布局 A/B 带宽实验**(专利交底《基于内存语义直接寻址的KV缓存组块读取》L1 实验):

HiSparse decode 只在设备侧保留热缓冲,每步按层组从池化存储取回 top-k 相关 token 的 KV。
同一份逻辑 KV 数据的两种物理布局,在传输层给出两种截然不同的 gather 形态:

- **布局 A(逐层,当前 sglang hisparse host 池形态)**:
  层 l 的 token t 位于 `layer_base[l] + t·s` —— f 个批(或 1 个交错批)、
  每批 k 段、每段 s 字节(MLA 切片,bf16 576+64 维 = 1280B);
- **布局 B(层组共置组块,交底书的 GBT 仿射布局)**:
  token t 的 f 层切片共置于 `GBT + t·(f·s)`,层 i 是 `+i·s` 切片偏移 ——
  1 个批、k 段、每段 f·s 字节(f=4 时 ≈5.1KB),
  即仿射地址 `A(g,t,i) = B + t·(f·s) + i·s` 的直接实现。

两种布局每轮搬运**相同的 k·f·s 字节**,A/B 对比隔离「取回单位/连续性」效应;
top-k 索引在模拟上下文窗口内随机(`--topk-pattern contiguous` 可分离随机性效应)。
预期验证(200G RoCE,25 GB/s 线速):s 字节逐层区利用率显著低于 30%,
f·s 字节组块区利用率 ≥60%(交底书主张)。

- 拓扑与 14 号一致(08 的守护骨架 × 09/14 的数据面):FAR 守护贡献一个 DRAM 槽,
  客户端以 `DEVICE_RDMA | DEVICE_SCHEDULE` 建池,AICore 内核直发 RDMA;
- 每 (f, s) 配置:双布局播种(同一逻辑内容、两种排列)→ checked 路径 warmup 逐布局校验 →
  **先所有布局 READ(swap-in 形态)、后所有布局 WRITE(eager-backup 形态;写会覆盖
  FAR 种子,故固定读先写后)** → CSV 落盘;
- 计时路径 `--variant v3`(默认,uniform 契约与 top-k gather 天然匹配:单 peer/单方向/
  src 与 dst 各在同一 mkey)/ `v2` / `checked`;`--graph` 将整轮提交捕获为 NPUGraph 重放
  (decode 图内嵌形态);
- 双口径指标:每 gather 墙钟(us,解码步预算视角)+ GB/s + 利用率 %(÷`--line-rate`);
  CSV 含 14 号口径(GiB)列,便于与历史矩阵直接对照。

## 拓扑与角色

```
FAR 节点(常驻守护,复用 08/14)             NEAR 节点(客户端)
┌────────────────────────────┐           ┌─────────────────────────────────────┐
│ contributor 贡献 DRAM slot │           │ 注册 HBM 张量 dst/staging(本地端点) │
│  区域A:逐层排列 region     │◄──────────│ device_copy_batch_v3/v2:            │
│  区域B:组块(GBT)排列region │ RDMA      │   READ  = FAR → HBM (swap-in)       │
│                            │  (AICore  │   WRITE = HBM → FAR (eager backup)  │
│                            │   自发)   │ top-k 随机索引 × {chunk, per_layer}  │
└────────────────────────────┘           └─────────────────────────────────────┘
```

## 快速开始(跨节点手工分别启动)

```bash
# 1) FAR 节点启动守护(与 08/14 相同),等到 "... memory contributors serving (...)"
python3 memfabric_daemon.py --store tcp://<far_ip>:8588 --devs 0,1

# 2) NEAR 节点跑客户端(默认: k=2048, f∈{1,2,4,8}, s=1280B, 三布局 × 双方向, v3)
MF_QPS_PER_PEER=4 python3 memfabric_client.py --store tcp://<far_ip>:8588 --dev 1

#    扩展块尺寸曲线(s ∈ {640,1280,2560} = KV8/bf16/宽幅):
python3 memfabric_client.py --store tcp://<far_ip>:8588 --dev 1 --s-list 640,1280,2560

#    图模式(decode 图内嵌形态)/ 随机性对照 / 仅读方向:
python3 memfabric_client.py --store tcp://<far_ip>:8588 --dev 1 --graph
python3 memfabric_client.py --store tcp://<far_ip>:8588 --dev 1 --topk-pattern contiguous
python3 memfabric_client.py --store tcp://<far_ip>:8588 --dev 1 --directions read

# 3) 用 CSV 做端到端推断(见 e2e_model.py)
python3 e2e_model.py --csv log/gather_results.csv

# 4) 停止守护
kill -TERM <daemon_pid>
```

## 关键参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--k` | 2048 | 每层组 top-k token 数(GLM-5.2 index_topk) |
| `--f-list` | 1,2,4,8 | 层组跨度扫描(f=4 对应 GLM-5.2 IndexShare;f=1 即逐层退化) |
| `--s-list` | 1280 | 每 (token,layer) 切片字节数,4 字节对齐(1280=bf16 MLA 576+64;640=KV8) |
| `--ctx-tokens` | 32768 | 模拟上下文窗口(token 数),决定 top-k 地址散布 |
| `--layouts` | 全部 | `chunk,per_layer_fbatch,per_layer_1batch`;fbatch=逐层 f 个批(最贴近现状语义),1batch=单批 f×k 段 |
| `--topk-pattern` | random | random=真实 top-k gather 形态;contiguous=分离随机性效应 |
| `--rounds` / `--round-bytes` | 0 / 64M | 每计时段轮数;0 = 按 64M 目标量自动(≥3) |
| `--variant` | v3 | v3=uniform 三数组 DVA / v2=路由五数组 / checked=host 逐段检查(64 段/launch) |
| `--graph` / `--replays` | 关 / 4 | 将整轮提交捕获为一张 NPUGraph 重放 |
| `--real-pool-size` | 2G | FAR 槽字节数,须覆盖 `2 × ctx × max(f) × max(s)`(默认扫参 670MB) |
| `--line-rate` | 25.0 | 利用率分母(decimal GB/s;200G RoCE 单向 25 GB/s) |
| `--csv` | `<run-dir>/gather_results.csv` | 结果 CSV(逐配置增量落盘,断跑不丢数据) |

环境变量与 14 号相同:`MF_QPS_PER_PEER`(多 QP lane)、`MF_DVA_DB_BATCH`(doorbell 批)。

## 必要条件
- 与 14 号完全相同:CANN ≥ 8.3.RC1、池须带 GB 对齐 HBM 窗口(固定 1G,承载 meta)、
  bisheng 安装期编译设备库、device RDMA 链路 UP、torch/torch_npu;
- 客户端不提交本地 DRAM 槽,本地端点全部为注册 HBM 张量(dst + 两个播种 staging)。

## 验收标准
- 客户端关键行依次出现:
  1. `device-scheduled pool created (DEVICE_RDMA | DEVICE_SCHEDULE, ...)`
  2. `remote block from FAR rank <R> ... layout regions: A=0x... B=0x...`
  3. 每配置:`regions seeded` → `warmup gather verify OK (all layouts, checked path)`
  4. 每 (f, s, layout, direction):`f=.. s=.. <layout>: <segs> segs x <B>B ...,
     read/write X GB/s (Y%), Z us/gather [verify OK | round-trip OK]`
  5. 末尾 A/B summary + `PPT L1 check ... per-layer util x% (claim: <30%) vs
     chunk util y% (claim: >=60%)`,以及 `[client] all configs OK`
- CSV 行数 = f 数 × s 数 × 布局数 × 方向数。

## 判读
- **同载荷 A/B**:每 (f, s) 行给出 per-layer(f×k 段 × s)vs chunk(k 段 × f·s)的
  读带宽/利用率与倍率——这是「布局效应」的净度量;
- **随机性效应**:同参数 `random` vs `contiguous` 两轮对照,差值即随机离散
  gather 本身的代价(相对 14 号的槽轮转连续序列);
- **f=1 行**是 s 字节纯块尺寸点;chunk 各 f 行给出块尺寸曲线(1280B→10KB),
  可直接回填交底书图 6「带宽利用率 vs 块尺寸」;
- **与 14 号历史数据对照**:CSV 的 `bw_gib_s_14conv` 列与 14 号打印同口径
  (注意:14 号标注 GB/s 实为 GiB/s;本示例 `bw_gb_s` 为 decimal,利用率一律按
  decimal 计算,与 25 GB/s 线速同量纲);
- **us/gather** 即单层组一次 swap-in 的墙钟(k=2048 满量程),直接喂给
  `e2e_model.py` 做解码步折算;miss 率折算、端到端 TPS 推断与敏感性扫描见
  `e2e_model.py --help`(其内置 2026-10-12 标定成本模型作 CSV 缺省回退)。
- **e2e 取数口径**:模型按 (布局类, seg_bytes, segs) 取 CSV 行,chunk 行键为
  `(f·s, k)`——若同一 CSV 同时扫 f 与 s 会撞键(如 f=2,s=2560 ≡ f=4,s=1280,
  取后写行),推断时务必用 `--f/--s` 指明单一组合;graph 行与 checked 行不参与。

## 排障
- 除 14 号排障表外,本用例特有:
  - create/extend 后报 `FAR slot too small: 2 layouts x ctx ...`:扫参超出
    `--real-pool-size`,按提示调大或缩扫参;
  - warmup 即报 `read layout/addressing mismatch`:双区域播种或布局寻址异常,
    优先怀疑 FAR 槽被上一轮污染(客户端退出后等 ~10s 再跑,同 14 号宽限自愈)。
