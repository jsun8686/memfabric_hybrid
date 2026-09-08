# 03_single_node_multi_process_ralloc

## 场景
单节点双进程 ralloc 最小闭环：rank0=NEAR（请求者，兼任 store 宿主，master 服务随其激活），
rank1=FAR（常驻贡献者，被动响应 JOIN_ALLOC）。

## 目标
验证 ralloc 完整链路：create 纯对齐（零本地提交）→ extend_local_mem（本地槽）→
extend_remote_mem（PLACEMENT → 跨进程 JOIN_ALLOC create-or-extend）→ 组成员查询 →
copy_data AUTO 方向往返 → 二次远端获取（executor extend 分支 + LB 记账刷新）→ destroy。

## 使用能力
`ralloc.initialize / create / extend_local_mem / extend_remote_mem / get_group_ranks /
get_mem_size_by_rank / get_mem_ptr_by_rank / copy_data / wait / destroy`。

## 规模建议
- 2 进程（worldSize=2），每 rank 窗槽 1GiB（仅 VA 预留）。
- 提交量：NEAR 本地 32MB + FAR 远端 64MB + 32MB，拷贝载荷 4MB。

## 必要条件
- 已安装 memfabric_hybrid whl（含 ralloc 子模块）。
- data_op_type=HOST_RDMA：单机进程间走本地路径，无需 NPU；无 RDMA 环境可改 HOST_TCP。

## 验收标准
- 输出 5/5 检查点全部通过，两子进程 exitcode=0。
- `get_group_ranks() == [0, 1]`；FAR 槽往返数据 torch.equal 校验通过。
- 结束时 `mf.get_last_err_msg() == ""`。

## 运行
```bash
python3 03_single_node_multi_process_ralloc.py
```
