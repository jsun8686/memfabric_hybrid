#!/usr/bin/env python3
# coding=utf-8
# Copyright: (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
"""Near-side device-scheduled RDMA under NPU graph capture (08 + DEVICE_SCHEDULE).

Same topology and skeleton as 08_far_daemon_near_client, but the pool is created with
DEVICE_RDMA | DEVICE_SCHEDULE on a DRAM window: the transport layer builds AI-core QPs
and publishes the meta/QP/MR context into the fixed device meta window, so an AICore
kernel (libmf_smem_ralloc_device_rdma.so, built at install time) drives the RDMA data
plane by itself. The host-side device_copy only enqueues the kernel, which makes the
copy job (one-sided WRITE/READ + quiet) NPU-graph capturable: capture once, replay K
times, zero host interaction per replay.
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
DEFAULT_SIZE = "1M"          # bytes per one-sided copy
DEFAULT_REPLAYS = 8          # graph replays after capture
DEFAULT_BLOCK_SIZE = "64M"   # DRAM slot bytes committed on each side
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


def _pattern(i, seed):
    return (seed * 1103515245 + i * 7) & 0xFFFFFFFF


def _write_u32_pattern(gva, words, seed):
    view = (ctypes.c_uint32 * words).from_address(gva)
    for i in range(words):
        view[i] = _pattern(i, seed)


def _check_u32_pattern(gva, words, seed):
    view = (ctypes.c_uint32 * words).from_address(gva)
    bad = 0
    for i in range(words):
        if view[i] != _pattern(i, seed):
            bad += 1
    return bad


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--store", required=True,
                        help="store url of the FAR daemon, e.g. tcp://10.0.0.1:8588")
    parser.add_argument("--dev", type=int, required=True,
                        help="NPU id this client runs on")
    parser.add_argument("--size", default=DEFAULT_SIZE,
                        help=f"bytes per one-sided copy (K/M/G suffix, default {DEFAULT_SIZE})")
    parser.add_argument("--replays", type=int, default=DEFAULT_REPLAYS,
                        help=f"graph replays after capture (default {DEFAULT_REPLAYS})")
    parser.add_argument("--block-size", default=DEFAULT_BLOCK_SIZE,
                        help=f"DRAM slot bytes committed on each side, must be >= 2 * size: "
                             f"slot layout is [pattern | verify] "
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
    if 2 * size > block:
        raise RuntimeError(f"--block-size ({block}) must be >= 2 * size ({2 * size}): "
                           f"slot layout is [pattern | verify]")
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

        # device-scheduled pool on a DRAM window: both sides commit their slot below.
        # max_hbm_size is GB aligned and hosts ONLY the fixed device meta window
        # (rank/QP/MR context); the copy slots live in the DRAM window
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
        _log(f"[client rank {rank}] remote block from FAR rank {far_rank} (gva=0x{far_gva:x}, npu {dev}), "
             f"local gva=0x{local_gva:x}")

        # slot layout: [0, size) pattern | [size, 2*size) verify
        words = size // 4
        seed = rank + 1
        _write_u32_pattern(local_gva, words, seed)
        ctypes.memset(local_gva + size, 0, size)

        # warmup round-trip on a side stream, OUTSIDE any graph: the first device_copy
        # dlopens and loads the kernel library, which is illegal inside capture
        side = torch.npu.Stream()
        side.wait_stream(torch.npu.current_stream())
        with torch.npu.stream(side):
            stream_ptr = torch.npu.current_stream().npu_stream
            assert handle.device_copy(local_gva, far_gva, size, stream_ptr) == 0, "warmup write failed"
            assert handle.device_copy(far_gva, local_gva + size, size, stream_ptr) == 0, \
                "warmup read failed"
        torch.npu.synchronize()
        assert _check_u32_pattern(local_gva + size, words, seed) == 0, "warmup round-trip mismatch"
        _log("[client] warmup round-trip OK (kernel library loaded, meta window reachable)")

        # capture the whole copy job into an NPU graph: one-sided WRITE + quiet
        graph = torch.npu.NPUGraph()
        torch.npu.synchronize()
        with torch.npu.stream(side):
            graph.capture_begin()
            assert handle.device_copy(local_gva, far_gva, size,
                                      torch.npu.current_stream().npu_stream) == 0, \
                "captured copy submit failed"
            graph.capture_end()
        _log(f"[client] graph captured: 1 x {size} byte device-scheduled WRITE + quiet, "
             f"no host interaction inside")

        t0 = time.perf_counter()
        for _ in range(args.replays):
            graph.replay()
        torch.npu.synchronize()
        t_replay = time.perf_counter() - t0
        moved = args.replays * size
        _log(f"[client] {args.replays} replays done: {moved / GIB:.2f} GiB in {t_replay * 1e3:.2f} ms, "
             f"{moved / t_replay / GIB:.2f} GB/s device-scheduled")

        # verify on the default stream: READ the far slot back into the verify area
        assert handle.device_copy(far_gva, local_gva + size, size, 0) == 0, "verify read failed"
        torch.npu.synchronize()
        bad = _check_u32_pattern(local_gva + size, words, seed)
        assert bad == 0, f"verify failed: {bad}/{words} words mismatch after {args.replays} replays"
        _log(f"[client] verify OK: far rank {far_rank} slot matches the pattern "
             f"({words} words, seed {seed})")

        handle.destroy()
        mf.get_and_clear_last_err_msg()
        assert mf.get_last_err_msg() == "", mf.get_last_err_msg()
        _log(f"[client rank {rank}] remote block released, exiting")
    finally:
        if ralloc_inited:
            ralloc.uninitialize(0)
        mf.uninitialize()
    _log("[client] device-scheduled RDMA under NPU graph finished cleanly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
