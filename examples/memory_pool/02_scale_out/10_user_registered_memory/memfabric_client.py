#!/usr/bin/env python3
# coding=utf-8
# Copyright: (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
"""User-registered NPU HBM tensors as device-scheduled copy endpoints (09 + register()).

Same topology and skeleton as 09_near_device_scheduled_rdma, but the local copy endpoints
are user torch tensors registered via handle.register(addr, size) instead of pool slots.
P1 contract: only NPU-managed HBM memory is accepted (the host rejects other addresses up
front), and the registered MR keeps regAddress == addr, so the AICore RDMA kernel addresses
the SGE directly under the user MR's lkey. Flow: register both tensors -> WRITE tensor ->
FAR pool slot -> READ FAR slot -> tensor2 -> verify -> capture/replay the WRITE ->
unregister -> expect the host precheck to reject further copies -> expect a host-DRAM
address to be rejected at register time.
"""

import argparse
import ctypes
import os
import socket
import sys
import time

import memfabric_hybrid as mf
from memfabric_hybrid import ralloc

DEFAULT_WORLD = 512          # declared world capacity, actual members join dynamically
NIC_PORT_BASE = 10010        # data-plane nic port base (set_nic); NOT the control rpc port
RPC_PORT_BASE = 11105        # control rpc port = base + rankId (smem_ralloc_def.h default)
DEFAULT_SIZE = "1M"          # bytes per tensor / per one-sided copy
DEFAULT_REPLAYS = 8          # graph replays after capture
DEFAULT_BLOCK_SIZE = "64M"   # DRAM slot bytes committed on each side (FAR landing zone)
DEFAULT_MAX_POOL_SIZE = "4G" # pool DRAM window, must be GB aligned (VMM segment rule)
META_HBM_WINDOW = 1 << 30    # minimal GB-aligned hbm window: hosts the fixed device meta window only
EXTEND_RETRY_SEC = 5
EXTEND_TIMEOUT_SEC = 300
GIB = 1 << 30

DATA_OP = ralloc.RallocDataOpType.DEVICE_RDMA | ralloc.RallocDataOpType.DEVICE_SCHEDULE
MEM_TYPE = ralloc.RallocMemType.HOST


_term_fd = None


def _log(msg):
    print(msg, flush=True)
    if _term_fd is not None:
        os.write(_term_fd, (msg + "\n").encode("utf-8", "replace"))


def _wait_tcp(url, timeout_sec):
    host, port = url.split("://", 1)[1].rsplit(":", 1)
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        with socket.socket() as s:
            if s.connect_ex((host, int(port))) == 0:
                return
        time.sleep(0.5)
    raise RuntimeError(f"endpoint not reachable within {timeout_sec}s: {url} — is the daemon up?")


def _parse_size(tok):
    tok = tok.strip().upper()
    mult = 1
    if tok.endswith("K"):
        mult, tok = 1024, tok[:-1]
    elif tok.endswith("M"):
        mult, tok = 1 << 20, tok[:-1]
    elif tok.endswith("G"):
        mult, tok = 1 << 30, tok[:-1]
    n = int(tok)
    if n <= 0:
        raise ValueError(f"bad size token: {tok}")
    return n * mult


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--store", required=True,
                        help="store url of the FAR daemon, e.g. tcp://10.0.0.1:8588")
    parser.add_argument("--dev", type=int, required=True,
                        help="NPU id this client runs on")
    parser.add_argument("--size", default=DEFAULT_SIZE,
                        help=f"bytes per tensor / per one-sided copy (K/M/G suffix, default {DEFAULT_SIZE})")
    parser.add_argument("--replays", type=int, default=DEFAULT_REPLAYS,
                        help=f"graph replays after capture (default {DEFAULT_REPLAYS})")
    parser.add_argument("--block-size", default=DEFAULT_BLOCK_SIZE,
                        help=f"DRAM slot bytes committed on each side as the FAR landing zone "
                             f"(K/M/G suffix, default {DEFAULT_BLOCK_SIZE})")
    parser.add_argument("--max-pool-size", default=DEFAULT_MAX_POOL_SIZE,
                        help=f"pool DRAM window declared to the FAR placement master, must be GB "
                             f"aligned (K/M/G suffix, default {DEFAULT_MAX_POOL_SIZE})")
    parser.add_argument("--world", type=int, default=DEFAULT_WORLD,
                        help=f"declared world capacity (default {DEFAULT_WORLD})")
    parser.add_argument("--rpc-port-base", type=int, default=RPC_PORT_BASE,
                        help=f"control rpc port base (default {RPC_PORT_BASE}); must match the daemon's value")
    parser.add_argument("--run-dir", default=None, help="client log dir (default ./log)")
    args = parser.parse_args()

    global _term_fd
    run_dir = os.path.abspath(args.run_dir or "./log")
    os.makedirs(run_dir, exist_ok=True)
    log = open(os.path.join(run_dir, f"near_dev{args.dev}.log"), "w")
    _term_fd = os.dup(1)
    os.dup2(log.fileno(), 1)
    os.dup2(log.fileno(), 2)
    _log(f"[client] run dir: {run_dir}, store: {args.store}, world: {args.world}, dev: {args.dev}")

    size = _parse_size(args.size)
    block = _parse_size(args.block_size)
    max_pool = _parse_size(args.max_pool_size)
    if size % 4 != 0:
        raise RuntimeError(f"--size ({size}) must be 4-byte aligned (int32 verify pattern)")
    if size > block:
        raise RuntimeError(f"--size ({size}) exceeds --block-size ({block})")
    if block > max_pool:
        raise RuntimeError(f"--block-size ({block}) exceeds --max-pool-size ({max_pool})")
    if max_pool % GIB != 0:
        raise RuntimeError(f"--max-pool-size ({max_pool}) must be GB aligned (VMM segment rule)")

    _wait_tcp(args.store, 30)
    dev = args.dev

    mf.set_log_level(1)
    assert mf.initialize() == 0, "mf.initialize failed"
    ralloc_inited = False
    try:
        import torch
        import torch_npu  # noqa: F401 — registers the npu backend and torch.npu.*

        cfg = ralloc.RallocConfig()
        cfg.auto_ranking = True
        cfg.role = ralloc.RallocRole.NEAR
        cfg.start_store = False
        cfg.dynamic_world_size = True
        cfg.rpc_port_base = args.rpc_port_base
        cfg.set_nic(f"tcp://{socket.gethostbyname(socket.gethostname())}:{NIC_PORT_BASE}")
        assert ralloc.initialize(args.store, args.world, dev, cfg) == 0, "ralloc.initialize failed"
        ralloc_inited = True
        rank = ralloc.get_rank_id()

        # device-scheduled pool on a DRAM window: the FAR side commits the landing slot.
        # max_hbm_size is GB aligned and hosts ONLY the fixed device meta window
        # (rank/QP/MR context); the user tensors below live in torch's own HBM allocator
        handle = ralloc.create(id=0, max_dram_size=max_pool, max_hbm_size=META_HBM_WINDOW, data_op_type=DATA_OP)
        _log(f"[client rank {rank}] device-scheduled pool created (DEVICE_RDMA | DEVICE_SCHEDULE)")

        ret, info = handle.extend_local_mem(MEM_TYPE, block)
        assert ret == 0 and info.get("gva"), f"extend_local_mem failed: {ret} {info}"
        local_gva = handle.get_mem_ptr_by_rank(rank, MEM_TYPE)
        assert local_gva != 0, "local DRAM slot not visible"

        deadline = time.time() + EXTEND_TIMEOUT_SEC
        while True:
            ret, info = handle.extend_remote_mem(MEM_TYPE, block)
            if ret == 0 and info.get("gva"):
                break
            if time.time() > deadline:
                raise RuntimeError(f"no FAR candidate within {EXTEND_TIMEOUT_SEC}s: {ret} {info}")
            time.sleep(EXTEND_RETRY_SEC)
        far_rank = info["rank_id"]
        far_gva = handle.get_mem_ptr_by_rank(far_rank, MEM_TYPE)
        assert far_gva != 0, "far DRAM slot not visible"
        mf.get_and_clear_last_err_msg()
        _log(f"[client rank {rank}] remote landing slot from FAR rank {far_rank} (gva=0x{far_gva:x}, npu {dev})")

        # ---- negative case 1: a host-DRAM address must be rejected at register time ----
        host_buf = ctypes.create_string_buffer(4096)
        ret = handle.register(ctypes.addressof(host_buf), 4096)
        assert ret != 0, "register() accepted a host-DRAM address: P1 must reject non-HBM memory"
        mf.get_and_clear_last_err_msg()
        _log("[client] host-DRAM register rejected as expected (P1: NPU HBM only)")

        # ---- user tensors as copy endpoints ----
        src = torch.empty(size, dtype=torch.uint8, device="npu")
        dst = torch.empty(size, dtype=torch.uint8, device="npu")
        src_addr = src.data_ptr()
        dst_addr = dst.data_ptr()
        assert src_addr != 0 and dst_addr != 0 and src_addr != dst_addr, "tensor allocation failed"
        words = size // 4
        seed = rank + 1
        expect = (torch.arange(words, dtype=torch.int32, device="npu") * 7 + seed)
        src.view(torch.int32).copy_(expect)
        dst.zero_()
        torch.npu.synchronize()
        _log(f"[client] tensors: src=0x{src_addr:x} dst=0x{dst_addr:x} ({size} bytes each, HBM)")

        assert handle.register(src_addr, size) == 0, "register(src tensor) failed"
        assert handle.register(dst_addr, size) == 0, "register(dst tensor) failed"
        _log("[client] both tensors registered (user MR table published to the meta window)")

        # warmup round-trip on a side stream, OUTSIDE any graph: the first device_copy
        # dlopens and loads the kernel library, which is illegal inside capture
        side = torch.npu.Stream()
        side.wait_stream(torch.npu.current_stream())
        with torch.npu.stream(side):
            stream_ptr = torch.npu.current_stream().npu_stream
            assert handle.device_copy(src_addr, far_gva, size, stream_ptr) == 0, "warmup write failed"
            assert handle.device_copy(far_gva, dst_addr, size, stream_ptr) == 0, "warmup read failed"
        torch.npu.synchronize()
        assert torch.equal(dst.view(torch.int32), expect), "warmup round-trip mismatch"
        _log("[client] warmup round-trip OK (tensor -> FAR slot -> tensor2, kernel library loaded)")

        # capture the WRITE into an NPU graph: user tensor -> FAR pool slot + quiet
        graph = torch.npu.NPUGraph()
        torch.npu.synchronize()
        with torch.npu.stream(side):
            graph.capture_begin()
            assert handle.device_copy(src_addr, far_gva, size,
                                      torch.npu.current_stream().npu_stream) == 0, \
                "captured copy submit failed"
            graph.capture_end()
        _log(f"[client] graph captured: 1 x {size} byte device-scheduled WRITE from the user tensor, "
             f"no host interaction inside")

        t0 = time.perf_counter()
        for _ in range(args.replays):
            graph.replay()
        torch.npu.synchronize()
        t_replay = time.perf_counter() - t0
        moved = args.replays * size
        _log(f"[client] {args.replays} replays done: {moved / GIB:.2f} GiB in {t_replay * 1e3:.2f} ms, "
             f"{moved / t_replay / GIB:.2f} GB/s device-scheduled")

        # verify on the default stream: READ the far slot back into the second tensor
        dst.zero_()
        assert handle.device_copy(far_gva, dst_addr, size, 0) == 0, "verify read failed"
        torch.npu.synchronize()
        assert torch.equal(dst.view(torch.int32), expect), \
            f"verify failed: tensor2 != pattern after {args.replays} replays"
        _log(f"[client] verify OK: FAR rank {far_rank} slot matches the tensor pattern "
             f"({words} words, seed {seed})")

        # ---- negative case 2: after unregister the copy must fail the host precheck ----
        assert handle.unregister(dst_addr) == 0, "unregister(dst tensor) failed"
        ret = handle.device_copy(far_gva, dst_addr, size, 0)
        assert ret != 0, "device_copy into an unregistered tensor must fail"
        mf.get_and_clear_last_err_msg()
        _log("[client] post-unregister copy rejected as expected (host precheck)")

        assert handle.unregister(src_addr) == 0, "unregister(src tensor) failed"
        del src, dst, expect
        handle.destroy()
        mf.get_and_clear_last_err_msg()
        assert mf.get_last_err_msg() == "", mf.get_last_err_msg()
        _log(f"[client rank {rank}] tensors unregistered, remote block released, exiting")
    finally:
        if ralloc_inited:
            ralloc.uninitialize(0)
        mf.uninitialize()
    _log("[client] user-registered HBM endpoints under NPU graph finished cleanly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
