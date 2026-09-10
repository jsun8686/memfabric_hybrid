# 04_multi_node_ralloc

## 场景
两节点 ralloc：head 节点跑 rank0=NEAR（请求者，兼任 store 宿主，master 服务随其激活），
node B 跑 rank1=FAR（常驻贡献者）。远端块物理落在 node B 的 host DRAM，head 经 GVA 直接读写。

## 目标
验证跨节点远端内存获取闭环：PLACEMENT（master LB 选点）→ JOIN_ALLOC（FAR 被动建池/贡献 +
join 融合）→ {rankId, gva} 交付 → 跨节点 copy_data AUTO 往返校验。

## 使用能力
`ralloc.initialize / create / extend_remote_mem / get_group_ranks / get_mem_size_by_rank /
copy_data / destroy`。

## 规模建议
- 每节点 1 进程（worldSize=2），每 rank 窗槽 1GiB（仅 VA 预留），远端块 64MB，拷贝载荷 4MB。

## 必要条件
- 两节点已安装同版本 memfabric_hybrid whl，网络互通。
- host 模式（默认）data_op_type=HOST_RDMA：需节点间 RDMA（RoCE/IB）NIC；无 RDMA 环境可改为 HOST_TCP（吞吐下降）。
- device 模式追加 `device` 参数：data_op_type=SDMA|DEVICE_RDMA，需 NPU+CANN（ASCEND_NPU 构建）环境；预留
  **HBM-only 窗**——A2(910B) SoC 上 DRAM 窗与 SDMA 位互斥（hybm conn-based DRAM 段无设备坐标，SDMA 不可达；
  910C/GVA_V4 统一编址解除）。

## 验收标准
- head 输出 "round-trip via FAR block OK"（远端块写读一致）且 `get_group_ranks() == [0, 1]`。
- Ctrl+C 后双侧干净退出，`mf.get_last_err_msg() == ""`。
- head 等待 FAR 注册期间，master 日志的 `no candidate for placement` 与 head 侧 "no FAR candidate
  yet ... retrying" 属预期噪音（重试环设计内失败，成功后 last error 已被清除）。

## 运行
启动顺序不严格（head 内置等待 FAR 注册的重试环），推荐先起 node B：
```bash
# node B (FAR 贡献者)
python3 04_multi_node_ralloc.py 1 <head_ip>
# head (NEAR 请求者 + store)
python3 04_multi_node_ralloc.py 0 <head_ip>
# HBM 介质变体（两节点均追加 device 参数）
python3 04_multi_node_ralloc.py 1 <head_ip> device
python3 04_multi_node_ralloc.py 0 <head_ip> device
```
