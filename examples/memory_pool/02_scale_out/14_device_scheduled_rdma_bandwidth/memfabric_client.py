#!/usr/bin/env python3
# coding=utf-8
# Copyright: (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
"""Device-scheduled RDMA bandwidth sweep: 08's multi-size skeleton on 09's data plane.

Same topology as 08_far_daemon_near_client (the FAR daemon contributes one DRAM slot,
the client takes it with extend_remote_mem), but the pool is created with
DEVICE_RDMA | DEVICE_SCHEDULE so an AICore kernel drives the RDMA data plane with zero
host interaction per copy. The local endpoints are USER-REGISTERED HBM tensors (no local
DRAM slot is committed, no extend_local_mem): device_copy pairs a registered tensor with
the FAR pool slot on the two legal legs (tensor -> FAR slot, FAR slot -> tensor), exactly
the legs 09's warmup proved.

Per io-size: register src/dst tensors, warm up OUTSIDE any graph (the first device_copy
dlopens the kernel library, which is illegal inside capture), then time one WRITE phase
and one READ phase of blocks = batch-size / size one-sided copies each:

  default (--graph):  capture a 1-copy NPUGraph per direction, replay x blocks
  --no-graph:         enqueue device_copy directly x blocks (rotating far offsets),
                      one synchronize at the end

Output lines match 08's format so runs are directly comparable.
"""

import argparse
import os
import socket
import sys
import time

import memfabric_hybrid as mf
from memfabric_hybrid import ralloc

DEFAULT_WORLD = 512          # declared world capacity, actual members join dynamically
NIC_PORT_BASE = 10005        # data-plane nic port base (set_nic); NOT the control rpc port
RPC_PORT_BASE = 11100        # control rpc port = base + rankId (smem_ralloc_def.h default)
DEFAULT_SIZES = "1M,2M,4M,8M"
DEFAULT_BATCH_SIZE = "64M"       # one-way copy volume per granularity per direction
DEFAULT_REAL_POOL_SIZE = "64M"   # FAR slot bytes (must cover the largest granularity)
DEFAULT_MAX_POOL_SIZE = "4G"     # pool DRAM window declared to the FAR placement master
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


def _aligned_npu_tensor(nbytes, device, fill=False):
    import torch
    buf = torch.empty((nbytes + 4096) // 4, dtype=torch.int32, device=device)
    off = ((-buf.data_ptr()) % 4096) // 4
    t = buf[off:off + nbytes // 4]
    if fill:
        torch.arange(nbytes // 4, dtype=torch.int32, device=device, out=t)
    return t


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--store", required=True,
                        help="store url of the FAR daemon, e.g. tcp://10.0.0.1:8588")
    parser.add_argument("--dev", type=int, required=True,
                        help="NPU id this client runs on")
    parser.add_argument("--io-sizes", default=DEFAULT_SIZES,
                        help=f"copy granularity list, 4-byte aligned (K/M/G suffix, "
                             f"default {DEFAULT_SIZES})")
    parser.add_argument("--batch-size", default=DEFAULT_BATCH_SIZE,
                        help=f"one-way copy volume per granularity per direction, "
                             f"blocks = batch-size / size (K/M/G suffix, "
                             f"default {DEFAULT_BATCH_SIZE})")
    parser.add_argument("--real-pool-size", default=DEFAULT_REAL_POOL_SIZE,
                        help=f"FAR slot bytes taken from the pool, must cover the largest size "
                             f"and fit within --max-pool-size (K/M/G suffix, "
                             f"default {DEFAULT_REAL_POOL_SIZE})")
    parser.add_argument("--max-pool-size", default=DEFAULT_MAX_POOL_SIZE,
                        help=f"pool DRAM window declared to the FAR placement master, must be GB "
                             f"aligned (K/M/G suffix, default {DEFAULT_MAX_POOL_SIZE})")
    parser.add_argument("--no-graph", action="store_true",
                        help="time direct device_copy enqueues (rotating far offsets, one "
                             "synchronize at the end) instead of NPUGraph capture + replay")
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
    mode = "direct" if args.no_graph else "graph"
    _log(f"[client] run dir: {run_dir}, store: {args.store}, world: {args.world}, dev: {args.dev}, "
         f"56bits_gva: {args.enable_56bits_gva}, timed mode: {mode}")

    sizes = [_parse_size(t) for t in args.io_sizes.split(",") if t.strip() != ""]
    if not sizes:
        raise RuntimeError("empty --io-sizes")
    for size in sizes:
        if size % 4 != 0:
            raise RuntimeError(f"io size {size} must be 4-byte aligned (int32 verify pattern)")
    remote_bytes = _parse_size(args.real_pool_size)
    batch_bytes = _parse_size(args.batch_size)
    max_pool_bytes = _parse_size(args.max_pool_size)
    if max(sizes) > remote_bytes:
        raise RuntimeError(f"largest io size {max(sizes)} exceeds --real-pool-size ({remote_bytes})")
    if remote_bytes > max_pool_bytes:
        raise RuntimeError(f"--real-pool-size ({remote_bytes}) exceeds --max-pool-size ({max_pool_bytes})")
    if max_pool_bytes % GIB != 0:
        raise RuntimeError(f"--max-pool-size ({max_pool_bytes}) must be GB aligned (VMM segment rule)")

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

        # device-scheduled pool on a DRAM window; max_hbm_size is GB aligned and hosts ONLY
        # the fixed device meta window (rank/QP/MR context). The NEAR side commits NO local
        # DRAM slot: every local endpoint below is a user-registered HBM tensor.
        handle = ralloc.create(id=0, max_dram_size=max_pool_bytes, max_hbm_size=META_HBM_WINDOW,
                               data_op_type=DATA_OP, enable_56bits_gva=args.enable_56bits_gva)
        _log(f"[client rank {rank}] device-scheduled pool created (DEVICE_RDMA | DEVICE_SCHEDULE, "
             f"56bits_gva={args.enable_56bits_gva})")

        # slot address comes from the extend return (the committed truth), never arithmetic
        deadline = time.time() + EXTEND_TIMEOUT_SEC
        while True:
            ret, info = handle.extend_remote_mem(MEM_TYPE, remote_bytes)
            if ret == 0 and info.get("gva"):
                break
            if time.time() > deadline:
                raise RuntimeError(f"no FAR candidate within {EXTEND_TIMEOUT_SEC}s: {ret} {info}")
            time.sleep(EXTEND_RETRY_SEC)
        gva = info["gva"]
        far_rank = info["rank_id"]
        mf.get_and_clear_last_err_msg()
        _log(f"[client rank {rank}] remote block from FAR rank {far_rank} (gva=0x{gva:x}, npu {dev})")

        device = f"npu:{dev}"
        side = torch.npu.Stream()
        for size in sizes:
            if size > remote_bytes:
                raise RuntimeError(f"size {size} exceeds remote block {remote_bytes}")
            words = size // 4
            seed = rank * 1000 + size
            src = _aligned_npu_tensor(size, device, fill=True)
            src += seed
            dst = _aligned_npu_tensor(size, device)
            src_addr, dst_addr = src.data_ptr(), dst.data_ptr()
            torch.npu.synchronize()
            assert handle.register(src_addr, size) == 0, "register src HBM failed"
            assert handle.register(dst_addr, size) == 0, "register dst HBM failed"

            # warmup on a side stream, OUTSIDE any graph: the first device_copy dlopens and
            # loads the kernel library, which is illegal inside capture. Seed the FAR slot
            # from the tensor, read it back and compare on device.
            side.wait_stream(torch.npu.current_stream())
            with torch.npu.stream(side):
                stream_ptr = torch.npu.current_stream().npu_stream
                assert handle.device_copy(src_addr, gva, size, stream_ptr) == 0, "warmup seed write failed"
                assert handle.device_copy(gva, dst_addr, size, stream_ptr) == 0, "warmup read back failed"
            torch.npu.synchronize()
            assert torch.equal(dst, src), "warmup round-trip mismatch (tensor -> FAR -> tensor)"
            _log(f"[client] size {size}: warmup round-trip OK (registered HBM <-> FAR slot, seed {seed})")

            blocks = max(1, batch_bytes // size)
            slots = max(1, remote_bytes // size)
            one_way = blocks * size
            dst_offs = [gva + (i % slots) * size for i in range(blocks)]

            if args.no_graph:
                dst.zero_()
                torch.npu.synchronize()
                t0 = time.perf_counter()
                for off in dst_offs:
                    assert handle.device_copy(src_addr, off, size, 0) == 0, "direct L2G failed"
                torch.npu.synchronize()
                tw = time.perf_counter() - t0
                t0 = time.perf_counter()
                for off in dst_offs:
                    assert handle.device_copy(off, dst_addr, size, 0) == 0, "direct G2L failed"
                torch.npu.synchronize()
                tr = time.perf_counter() - t0
            else:
                torch.npu.synchronize()
                wgraph = torch.npu.NPUGraph()
                with torch.npu.stream(side):
                    wgraph.capture_begin()
                    assert handle.device_copy(src_addr, gva, size,
                                              torch.npu.current_stream().npu_stream) == 0, \
                        "captured write submit failed"
                    wgraph.capture_end()
                dst.zero_()
                torch.npu.synchronize()
                rgraph = torch.npu.NPUGraph()
                with torch.npu.stream(side):
                    rgraph.capture_begin()
                    assert handle.device_copy(gva, dst_addr, size,
                                              torch.npu.current_stream().npu_stream) == 0, \
                        "captured read submit failed"
                    rgraph.capture_end()
                torch.npu.synchronize()
                t0 = time.perf_counter()
                for _ in range(blocks):
                    wgraph.replay()
                torch.npu.synchronize()
                tw = time.perf_counter() - t0
                t0 = time.perf_counter()
                for _ in range(blocks):
                    rgraph.replay()
                torch.npu.synchronize()
                tr = time.perf_counter() - t0
                del wgraph, rgraph

            assert torch.equal(dst, src), f"round-trip mismatch at size {size}"
            _log(f"[client] size {size}: {blocks} blocks ({mode}), "
                 f"write {one_way / tw / GIB:.2f} GB/s ({tw / blocks * 1e6:.2f} us/block), "
                 f"read {one_way / tr / GIB:.2f} GB/s ({tr / blocks * 1e6:.2f} us/block) [round-trip OK]")

            assert handle.unregister(src_addr) == 0, "unregister src failed"
            assert handle.unregister(dst_addr) == 0, "unregister dst failed"
            del src, dst

        handle.destroy()
        mf.get_and_clear_last_err_msg()
        assert mf.get_last_err_msg() == "", mf.get_last_err_msg()
        _log(f"[client rank {rank}] tensors unregistered, remote block released, exiting")
    finally:
        if ralloc_inited:
            ralloc.uninitialize(0)
        mf.uninitialize()
    _log("[client] all sizes OK, client finished cleanly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
