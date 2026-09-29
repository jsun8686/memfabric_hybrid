#!/usr/bin/env python3
# coding=utf-8
# Copyright: (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
"""User-registered host-DRAM buffers as device-scheduled copy endpoints (10 + host memory).

Same topology and skeleton as 10_user_registered_memory, but the local copy endpoints are
4K-aligned anonymous mmap buffers on the host instead of NPU HBM tensors. The registered
host-DRAM MR carries a distinct device-dma base (HalHostRegister iova) in the v2 user-MR
table (regAddress at entry byte offset +24), so the AICore RDMA kernel addresses the SGE
as regAddress + (localAddr - addr) under the MR's lkey.

56-bit GVA contract: pool-slot GVA addresses form device-copy endpoints only -- never
dereference them on the CPU (with --enable-56bits-gva the GVA window lives above 2^55 and
is not CPU mapped). The user-registered buffers are the CPU-side surface of this example.

History: the probe matrix that once lived here (A/B/B4/B5/B6/B6b/B7/B8/C2) located the
regAddress serialization offset bug (host wrote +32, kernel read +24 -> fallback to the
raw user VA -> NIC zeros on every host-DRAM endpoint; HBM was masked by its identity
regAddress). The matrix was removed after the fix; see README for the record.

Flow: alignment negatives -> register src/dst -> round-trip -> capture/replay the WRITE ->
unregister -> expect the host precheck to reject further copies.
"""

import argparse
import ctypes
import mmap
import os
import socket
import sys
import time

import memfabric_hybrid as mf
from memfabric_hybrid import ralloc

DEFAULT_WORLD = 512          # declared world capacity, actual members join dynamically
NIC_PORT_BASE = 10005        # data-plane nic port base (set_nic); NOT the control rpc port
RPC_PORT_BASE = 11100        # control rpc port = base + rankId (smem_ralloc_def.h default)
DEFAULT_SIZE = "1M"          # bytes per buffer / per one-sided copy (must be 4K aligned)
DEFAULT_REPLAYS = 8          # graph replays after capture
DEFAULT_BLOCK_SIZE = "64M"   # DRAM slot bytes committed on each side (FAR landing zone)
DEFAULT_MAX_POOL_SIZE = "4G" # pool DRAM window, must be GB aligned (VMM segment rule)
META_HBM_WINDOW = 1 << 30    # minimal GB-aligned hbm window: hosts the fixed device meta window only
EXTEND_RETRY_SEC = 5
EXTEND_TIMEOUT_SEC = 300
GIB = 1 << 30
DRAM_ALIGN = 4096

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


def _map_buffer(size):
    """anonymous page-aligned mapping: the kernel guarantees 4K alignment for mmap bases.
    The ctypes view is transient so the mapping carries no long-lived buffer export --
    mm.close() then works on every exit path (no BufferError after exceptions)."""
    mm = mmap.mmap(-1, size)
    addr = ctypes.addressof(ctypes.c_char.from_buffer(mm))
    assert addr % DRAM_ALIGN == 0, f"mmap base 0x{addr:x} not page aligned"
    return mm, addr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--store", required=True,
                        help="store url of the FAR daemon, e.g. tcp://10.0.0.1:8588")
    parser.add_argument("--dev", type=int, required=True,
                        help="NPU id this client runs on")
    parser.add_argument("--size", default=DEFAULT_SIZE,
                        help=f"bytes per buffer / per one-sided copy, 4K aligned (K/M/G suffix, "
                             f"default {DEFAULT_SIZE})")
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
    parser.add_argument("--enable-56bits-gva", action="store_true",
                        help="create the pool with 56-bit GVA (GVA window above 2^55; slot GVA "
                             "addresses stay device-copy endpoints only, never CPU pointers)")
    parser.add_argument("--run-dir", default=None, help="client log dir (default ./log)")
    args = parser.parse_args()

    global _term_fd
    run_dir = os.path.abspath(args.run_dir or "./log")
    os.makedirs(run_dir, exist_ok=True)
    log = open(os.path.join(run_dir, f"near_dev{args.dev}.log"), "w")
    _term_fd = os.dup(1)
    os.dup2(log.fileno(), 1)
    os.dup2(log.fileno(), 2)
    _log(f"[client] run dir: {run_dir}, store: {args.store}, world: {args.world}, dev: {args.dev}, "
         f"56bits_gva: {args.enable_56bits_gva}")

    size = _parse_size(args.size)
    block = _parse_size(args.block_size)
    max_pool = _parse_size(args.max_pool_size)
    if size % DRAM_ALIGN != 0:
        raise RuntimeError(f"--size ({size}) must be {DRAM_ALIGN} aligned (host-DRAM MR rule)")
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
    src_mm = dst_mm = None
    try:
        import numpy as np
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
        # (rank/QP/MR context); the user buffers below live in anonymous host mappings
        handle = ralloc.create(id=0, max_dram_size=max_pool, max_hbm_size=META_HBM_WINDOW,
                               data_op_type=DATA_OP, enable_56bits_gva=args.enable_56bits_gva)
        _log(f"[client rank {rank}] device-scheduled pool created (DEVICE_RDMA | DEVICE_SCHEDULE, "
             f"56bits_gva={args.enable_56bits_gva})")

        # slot addresses come from the extend returns (the committed truth): the arithmetic
        # get_mem_ptr_by_rank (base + rank * maxSize) assumes every member's slot sits at
        # the same window base, which only holds while the FAR daemon's LVA layout is fresh
        ret, info = handle.extend_local_mem(MEM_TYPE, block)
        assert ret == 0 and info.get("gva"), f"extend_local_mem failed: {ret} {info}"
        local_gva = info["gva"]
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
        far_gva = info["gva"]
        assert far_gva != 0, "far DRAM slot not visible"
        mf.get_and_clear_last_err_msg()
        _log(f"[client rank {rank}] remote landing slot from FAR rank {far_rank} (gva=0x{far_gva:x}, npu {dev})")

        # ---- negative case 1: an unaligned host-DRAM address must be rejected at register time ----
        neg_mm, neg_base = _map_buffer(2 * DRAM_ALIGN)
        unaligned_addr = neg_base + DRAM_ALIGN + 128
        ret = handle.register(unaligned_addr, DRAM_ALIGN)
        assert ret != 0, "register() accepted an unaligned host-DRAM address: must be 4K aligned"
        mf.get_and_clear_last_err_msg()
        ret = handle.register(neg_base, DRAM_ALIGN // 2)  # aligned addr, unaligned size
        assert ret != 0, "register() accepted an unaligned host-DRAM size: must be a 4K multiple"
        mf.get_and_clear_last_err_msg()
        neg_mm.close()
        _log("[client] unaligned host-DRAM register rejected as expected (4K alignment rule)")

        # ---- host buffers as copy endpoints ----
        src_mm, src_addr = _map_buffer(size)
        dst_mm, dst_addr = _map_buffer(size)
        assert src_addr != dst_addr, "buffer mapping failed"
        words = size // 4
        seed = rank + 1
        expect = np.arange(words, dtype=np.int32) * 7 + seed
        np.frombuffer(src_mm, dtype=np.int32)[:] = expect
        np.frombuffer(dst_mm, dtype=np.int32)[:] = 0
        _log(f"[client] host buffers: src=0x{src_addr:x} dst=0x{dst_addr:x} "
             f"({size} bytes each, page-aligned anonymous mmap)")
        assert handle.register(src_addr, size) == 0, "register(src buffer) failed"
        assert handle.register(dst_addr, size) == 0, "register(dst buffer) failed"
        _log("[client] src/dst registered (user MR table v2: regAddress = HalHostRegister iova)")

        # round-trip user -> FAR -> user2 (device_copy submits on the side stream; the first
        # call also dlopens the kernel library, which is illegal inside graph capture)
        side = torch.npu.Stream()
        side.wait_stream(torch.npu.current_stream())
        with torch.npu.stream(side):
            stream_ptr = torch.npu.current_stream().npu_stream
            assert handle.device_copy(src_addr, far_gva, size, stream_ptr) == 0, "write failed"
            assert handle.device_copy(far_gva, dst_addr, size, stream_ptr) == 0, "read failed"
        torch.npu.synchronize()
        got = np.frombuffer(bytes(dst_mm[:]), dtype=np.int32)
        assert np.array_equal(got, expect), \
            f"round-trip mismatch: got={got[:8].tolist()} want={expect[:8].tolist()}"
        _log("[client] round-trip OK (host buffer -> FAR slot -> host buffer2)")

        # capture the WRITE into an NPU graph: host buffer -> FAR pool slot + quiet
        graph = torch.npu.NPUGraph()
        torch.npu.synchronize()
        with torch.npu.stream(side):
            graph.capture_begin()
            assert handle.device_copy(src_addr, far_gva, size,
                                      torch.npu.current_stream().npu_stream) == 0, \
                "captured copy submit failed"
            graph.capture_end()
        _log(f"[client] graph captured: 1 x {size} byte device-scheduled WRITE from the host buffer, "
             f"no host interaction inside")

        t0 = time.perf_counter()
        for _ in range(args.replays):
            graph.replay()
        torch.npu.synchronize()
        t_replay = time.perf_counter() - t0
        moved = args.replays * size
        _log(f"[client] {args.replays} replays done: {moved / GIB:.2f} GiB in {t_replay * 1e3:.2f} ms, "
             f"{moved / t_replay / GIB:.2f} GB/s device-scheduled")

        # verify on the default stream: READ the far slot back into the second buffer
        np.frombuffer(dst_mm, dtype=np.int32)[:] = 0
        assert handle.device_copy(far_gva, dst_addr, size, 0) == 0, "verify read failed"
        torch.npu.synchronize()
        got = np.frombuffer(bytes(dst_mm[:]), dtype=np.int32)
        assert np.array_equal(got, expect), \
            f"verify failed: buffer2 != pattern after {args.replays} replays"
        _log(f"[client] verify OK: FAR rank {far_rank} slot matches the host pattern "
             f"({words} words, seed {seed})")

        # ---- negative case 2: after unregister the copy must fail the host precheck ----
        assert handle.unregister(dst_addr) == 0, "unregister(dst buffer) failed"
        ret = handle.device_copy(far_gva, dst_addr, size, 0)
        assert ret != 0, "device_copy into an unregistered buffer must fail"
        mf.get_and_clear_last_err_msg()
        _log("[client] post-unregister copy rejected as expected (host precheck)")

        assert handle.unregister(src_addr) == 0, "unregister(src buffer) failed"
        dst_mm.close()
        src_mm.close()
        dst_mm = src_mm = None
        handle.destroy()
        mf.get_and_clear_last_err_msg()
        assert mf.get_last_err_msg() == "", mf.get_last_err_msg()
        _log(f"[client rank {rank}] buffers unregistered, remote block released, exiting")
    finally:
        if dst_mm is not None:
            dst_mm.close()
        if src_mm is not None:
            src_mm.close()
        if ralloc_inited:
            ralloc.uninitialize(0)
        mf.uninitialize()
    _log("[client] user-registered host-DRAM endpoints under NPU graph finished cleanly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
