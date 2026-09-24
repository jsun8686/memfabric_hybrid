#!/usr/bin/env python3
# coding=utf-8
# Copyright: (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
"""Pool multi-slice expansion: two extend_remote_mem slots on one pool, device-scheduled.

Same topology as 09/10, but the pool is grown twice: a second extend_local_mem and a second
extend_remote_mem give the SAME pool two block MRs per rank. That pushes ConnectRankInfo's
memoryMap to two entries per rank, and the device-visible QP/MR table must expose both
(MR_SLOTS_PER_RANK slots per rank; a range lookup picks the covering slot). Flow: extend
local slots 1/2 + remote slots 1/2 -> host-load two distinct patterns into the local slots
-> warmup WRITE both local->far pairs -> capture BOTH writes in one NPU graph -> replays ->
zero the local slots -> READ both far slots back -> verify each pattern came back through
its own slice (a slot mix-up would cross the patterns) -> clean destroy.
"""

import argparse
import os
import socket
import sys
import time

import numpy as np

import memfabric_hybrid as mf
from memfabric_hybrid import ralloc

DEFAULT_WORLD = 512          # declared world capacity, actual members join dynamically
NIC_PORT_BASE = 10010        # data-plane nic port base (set_nic); NOT the control rpc port
RPC_PORT_BASE = 11105        # control rpc port = base + rankId (smem_ralloc_def.h default)
DEFAULT_SIZE = "1M"          # bytes per pattern / per one-sided copy
DEFAULT_REPLAYS = 8          # graph replays after capture
DEFAULT_BLOCK_SIZE = "64M"   # DRAM slot bytes committed per extend (x2 local, x2 remote)
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


def _extend_remote(handle, block):
    deadline = time.time() + EXTEND_TIMEOUT_SEC
    while True:
        ret, info = handle.extend_remote_mem(MEM_TYPE, block)
        if ret == 0 and info.get("gva"):
            return info
        if time.time() > deadline:
            raise RuntimeError(f"no FAR candidate within {EXTEND_TIMEOUT_SEC}s: {ret} {info}")
        time.sleep(EXTEND_RETRY_SEC)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--store", required=True,
                        help="store url of the FAR daemon, e.g. tcp://10.0.0.1:8588")
    parser.add_argument("--dev", type=int, required=True,
                        help="NPU id this client runs on")
    parser.add_argument("--size", default=DEFAULT_SIZE,
                        help=f"bytes per pattern / per one-sided copy (K/M/G suffix, default {DEFAULT_SIZE})")
    parser.add_argument("--replays", type=int, default=DEFAULT_REPLAYS,
                        help=f"graph replays after capture (default {DEFAULT_REPLAYS})")
    parser.add_argument("--block-size", default=DEFAULT_BLOCK_SIZE,
                        help=f"DRAM slot bytes committed per extend, twice per side "
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
    if 2 * block > max_pool:
        raise RuntimeError(f"two blocks ({2 * block}) exceed --max-pool-size ({max_pool})")
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

        # device-scheduled pool on a DRAM window, grown twice per side below; max_hbm_size is GB
        # aligned and hosts ONLY the fixed device meta window (rank/QP/MR context)
        handle = ralloc.create(id=0, max_dram_size=max_pool, max_hbm_size=META_HBM_WINDOW, data_op_type=DATA_OP)
        _log(f"[client rank {rank}] device-scheduled pool created (DEVICE_RDMA | DEVICE_SCHEDULE)")

        # ---- grow the SAME pool: two local slices + two FAR slices ----
        local_gvas = []
        for i in (1, 2):
            ret, info = handle.extend_local_mem(MEM_TYPE, block)
            assert ret == 0 and info.get("gva"), f"extend_local_mem #{i} failed: {ret} {info}"
            local_gvas.append(info["gva"])
        far_rank = None
        far_gvas = []
        for i in (1, 2):
            info = _extend_remote(handle, block)
            gva = info["gva"]
            if far_rank is None:
                far_rank = info["rank_id"]
            else:
                assert info["rank_id"] == far_rank, \
                    f"second slice landed on rank {info['rank_id']}, expected {far_rank}"
            for other in far_gvas:
                assert not (gva < other + block and other < gva + block), \
                    f"slice gvas overlap: 0x{gva:x} vs 0x{other:x}"
            far_gvas.append(gva)
        mf.get_and_clear_last_err_msg()
        assert len(far_gvas) == 2, "expected two remote slices"
        lgva1, lgva2 = local_gvas
        _log(f"[client rank {rank}] pool grown to 2+2 slices: FAR rank {far_rank} slots "
             f"[0x{far_gvas[0]:x}, 0x{far_gvas[1]:x}), local slots [0x{lgva1:x}, 0x{lgva2:x}), "
             f"{block} bytes each")

        # ---- two distinct host patterns -> local slots (host copy path) ----
        words = size // 4
        pat1 = (np.arange(words, dtype=np.int32) * 7 + rank + 1)
        pat2 = (np.arange(words, dtype=np.int32) * 13 + rank + 101)
        assert handle.copy_data(pat1.ctypes.data, lgva1, size, 0) == 0, "host load pattern1 failed"
        assert handle.copy_data(pat2.ctypes.data, lgva2, size, 0) == 0, "host load pattern2 failed"
        _log(f"[client] patterns loaded: {size} bytes each (seeds {rank + 1} / {rank + 101})")

        # warmup both writes on a side stream, OUTSIDE any graph: the first device_copy dlopens
        # and loads the kernel library, which is illegal inside capture
        side = torch.npu.Stream()
        side.wait_stream(torch.npu.current_stream())
        with torch.npu.stream(side):
            stream_ptr = torch.npu.current_stream().npu_stream
            assert handle.device_copy(lgva1, far_gvas[0], size, stream_ptr) == 0, "warmup write1 failed"
            assert handle.device_copy(lgva2, far_gvas[1], size, stream_ptr) == 0, "warmup write2 failed"
        torch.npu.synchronize()
        _log("[client] warmup writes OK (kernel library loaded, both slices reachable)")

        # capture BOTH writes into one NPU graph: local slice -> matching FAR slice + quiet
        graph = torch.npu.NPUGraph()
        torch.npu.synchronize()
        with torch.npu.stream(side):
            graph.capture_begin()
            assert handle.device_copy(lgva1, far_gvas[0], size,
                                      torch.npu.current_stream().npu_stream) == 0, \
                "captured copy1 submit failed"
            assert handle.device_copy(lgva2, far_gvas[1], size,
                                      torch.npu.current_stream().npu_stream) == 0, \
                "captured copy2 submit failed"
            graph.capture_end()
        _log(f"[client] graph captured: 2 x {size} byte device-scheduled WRITEs, one per slice, "
             f"no host interaction inside")

        t0 = time.perf_counter()
        for _ in range(args.replays):
            graph.replay()
        torch.npu.synchronize()
        t_replay = time.perf_counter() - t0
        moved = args.replays * 2 * size
        _log(f"[client] {args.replays} replays done: {moved / GIB:.2f} GiB in {t_replay * 1e3:.2f} ms, "
             f"{moved / t_replay / GIB:.2f} GB/s device-scheduled")

        # ---- verify: zero the local slots, READ both FAR slices back, compare ----
        zero = np.zeros(words, dtype=np.int32)
        assert handle.copy_data(zero.ctypes.data, lgva1, size, 0) == 0, "zero local1 failed"
        assert handle.copy_data(zero.ctypes.data, lgva2, size, 0) == 0, "zero local2 failed"
        assert handle.device_copy(far_gvas[0], lgva1, size, 0) == 0, "verify read1 failed"
        assert handle.device_copy(far_gvas[1], lgva2, size, 0) == 0, "verify read2 failed"
        torch.npu.synchronize()
        back1 = np.empty(words, dtype=np.int32)
        back2 = np.empty(words, dtype=np.int32)
        assert handle.copy_data(lgva1, back1.ctypes.data, size, 0) == 0, "host fetch1 failed"
        assert handle.copy_data(lgva2, back2.ctypes.data, size, 0) == 0, "host fetch2 failed"
        assert np.array_equal(back1, pat1), "verify failed: slice1 pattern mismatch (MR slot mix-up?)"
        assert np.array_equal(back2, pat2), "verify failed: slice2 pattern mismatch (MR slot mix-up?)"
        _log(f"[client] verify OK: both FAR slices carry their own pattern after {args.replays} replays "
             f"(kernel MR-slot lookup picked the covering slot for each endpoint)")

        handle.destroy()
        mf.get_and_clear_last_err_msg()
        assert mf.get_last_err_msg() == "", mf.get_last_err_msg()
        _log(f"[client rank {rank}] pool destroyed, exiting")
    finally:
        if ralloc_inited:
            ralloc.uninitialize(0)
        mf.uninitialize()
    _log("[client] pool multi-slice device-scheduled copies under NPU graph finished cleanly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
