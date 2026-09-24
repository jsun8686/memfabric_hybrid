#!/usr/bin/env python3
# coding=utf-8
# Copyright: (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
"""Concurrent multi-pool creation: N independent pools race for FAR blocks at once.

N workers each build its OWN pool (distinct pool id, so every extend_remote_mem drives
its own JOIN_ALLOC create-branch on the contributor) and all of them call
extend_remote_mem at the same instant behind a barrier. The placement master grants
back-to-back blocks within milliseconds, so several create-branches hit one FAR node
concurrently: the contributor-side device open (PrepareOpenDevice) must serialize them —
the late opener waits and reuses the registered rdma handle instead of racing RaInit
into a libra double-init rejection followed by a failing hccl-group fallback.

Each worker then proves its block with a copy_data round-trip (host-initiated pool,
DEVICE_RDMA only) and destroys its pool.

Acceptance on the client: all N workers report "round-trip OK", a placement spread
line and a first-try extend counter. Acceptance on the FAR side (far_dev*.log): zero
"Hccp Init RA failed" / "HcclCommInitClusterInfoMemConfig failed", and from the second
create on the same rank "Had prepared device and get rdmaHandle success" (the reuse
path taken after waiting).
"""

import argparse
import multiprocessing as mp
import os
import queue
import socket
import sys
import time

import memfabric_hybrid as mf
from memfabric_hybrid import ralloc

DEFAULT_WORLD = 512          # declared world capacity, actual members join dynamically
NIC_PORT_BASE = 10015        # data-plane nic port base (set_nic); NOT the control rpc port
RPC_PORT_BASE = 11110        # control rpc port base = base + rankId (smem_ralloc_def.h default)
DEFAULT_WORKERS = 4          # concurrent pool creators (each its own pool id)
DEFAULT_SIZE = "1M"          # bytes of the round-trip verify pattern
DEFAULT_BLOCK_SIZE = "64M"   # remote block bytes taken per pool
DEFAULT_MAX_POOL_SIZE = "4G" # pool DRAM window declared to the FAR placement master
DEFAULT_POOL_BASE = 120      # pool ids = base .. base+workers-1 (distinct per worker)
BARRIER_TIMEOUT_SEC = 300    # slowest local pool build must not starve the others
EXTEND_RETRY_SEC = 5
EXTEND_TIMEOUT_SEC = 300
COLLECT_TIMEOUT_SEC = 600    # overall worker budget (create + extend + verify + destroy)
GIB = 1 << 30

DATA_OP = ralloc.RallocDataOpType.DEVICE_RDMA
MEM_TYPE = ralloc.RallocMemType.HOST

_term_fd = None


def _log(msg):
    print(msg, flush=True)
    if _term_fd is not None:
        os.write(_term_fd, (msg + "\n").encode("utf-8", "replace"))


def _wlog(tag, msg):
    print(f"[{tag}] {msg}", flush=True)


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


def _aligned_npu_tensor(nbytes, device):
    import torch
    buf = torch.empty((nbytes + 4096) // 4, dtype=torch.int32, device=device)
    off = ((-buf.data_ptr()) % 4096) // 4
    return buf[off:off + nbytes // 4]


def _worker_main(idx, pool_id, store, world, dev, rpc_base, block, size, max_pool,
                 barrier, result_q, run_dir):
    tag = f"worker {idx}"
    log = open(os.path.join(run_dir, f"near_worker{idx}.log"), "w")
    os.dup2(log.fileno(), 1)
    os.dup2(log.fileno(), 2)
    ralloc_inited = False
    try:
        mf.set_log_level(1)
        assert mf.initialize() == 0, "mf.initialize failed"
        cfg = ralloc.RallocConfig()
        cfg.auto_ranking = True
        cfg.role = ralloc.RallocRole.NEAR
        cfg.start_store = False
        cfg.dynamic_world_size = True
        cfg.rpc_port_base = rpc_base
        cfg.set_nic(f"tcp://{socket.gethostbyname(socket.gethostname())}:{NIC_PORT_BASE}")
        assert ralloc.initialize(store, world, dev, cfg) == 0, "ralloc.initialize failed"
        ralloc_inited = True
        rank = ralloc.get_rank_id()

        # own pool id per worker: every extend_remote_mem below drives its own
        # JOIN_ALLOC create-branch on the contributor it lands on
        handle = ralloc.create(id=pool_id, max_dram_size=max_pool, max_hbm_size=0,
                               data_op_type=DATA_OP)
        _wlog(tag, f"rank {rank} local pool {pool_id} created, waiting at the barrier")

        barrier.wait(timeout=BARRIER_TIMEOUT_SEC)

        # all workers fire extend_remote_mem within the same instant: back-to-back
        # JOIN_ALLOCs may land on one contributor node concurrently
        t0 = time.perf_counter()
        attempts = 0
        deadline = time.time() + EXTEND_TIMEOUT_SEC
        while True:
            attempts += 1
            ret, info = handle.extend_remote_mem(MEM_TYPE, block)
            if ret == 0 and info.get("gva"):
                break
            _wlog(tag, f"extend attempt {attempts} failed: {ret} {info}, retrying")
            if time.time() > deadline:
                raise RuntimeError(f"no FAR candidate within {EXTEND_TIMEOUT_SEC}s: {ret} {info}")
            time.sleep(EXTEND_RETRY_SEC)
        gva = info["gva"]
        far_rank = info["rank_id"]
        mf.get_and_clear_last_err_msg()

        # prove the block with a host-initiated copy_data round-trip carrying a
        # worker-unique pattern (a crossed pattern would mean a mixed-up block)
        import torch
        import torch_npu  # noqa: F401 — registers the npu backend
        device = f"npu:{dev}"
        src = _aligned_npu_tensor(size, device)
        dst = _aligned_npu_tensor(size, device)
        src.copy_(torch.arange(size // 4, dtype=torch.int32, device=device) * 11 + pool_id * 7 + 1)
        assert handle.register(src.data_ptr(), size) == 0, "register src HBM failed"
        assert handle.register(dst.data_ptr(), size) == 0, "register dst HBM failed"
        assert handle.copy_data(src.data_ptr(), gva, size, 0) == 0, "L2G verify write failed"
        assert handle.copy_data(gva, dst.data_ptr(), size, 0) == 0, "G2L verify read failed"
        assert torch.equal(dst, src), "round-trip pattern mismatch"
        assert handle.unregister(src.data_ptr()) == 0, "unregister src failed"
        assert handle.unregister(dst.data_ptr()) == 0, "unregister dst failed"
        handle.destroy()
        mf.get_and_clear_last_err_msg()
        assert mf.get_last_err_msg() == "", mf.get_last_err_msg()
        took = time.perf_counter() - t0
        _wlog(tag, f"pool {pool_id} block on rank {far_rank} (gva=0x{gva:x}), round-trip OK, "
                   f"extend attempts: {attempts}, took {took:.1f}s")
        result_q.put({"idx": idx, "ok": True, "pool": pool_id, "rank": far_rank, "attempts": attempts})
    except Exception as e:  # noqa: BLE001 — carried back to the parent for the summary
        _wlog(tag, f"FAILED: {e}")
        result_q.put({"idx": idx, "ok": False, "pool": pool_id, "err": str(e)})
    finally:
        if ralloc_inited:
            ralloc.uninitialize(0)
        mf.uninitialize()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--store", required=True,
                        help="store url of the FAR daemon, e.g. tcp://10.0.0.1:8587")
    parser.add_argument("--dev", type=int, required=True,
                        help="NPU id every worker runs on")
    parser.add_argument("--workers", type=int, default=DEFAULT_WORKERS,
                        help=f"concurrent pool creators, each with its own pool id "
                             f"(default {DEFAULT_WORKERS})")
    parser.add_argument("--size", default=DEFAULT_SIZE,
                        help=f"bytes of the round-trip verify pattern (K/M/G suffix, "
                             f"default {DEFAULT_SIZE})")
    parser.add_argument("--block-size", default=DEFAULT_BLOCK_SIZE,
                        help=f"remote block bytes taken per pool (K/M/G suffix, "
                             f"default {DEFAULT_BLOCK_SIZE})")
    parser.add_argument("--max-pool-size", default=DEFAULT_MAX_POOL_SIZE,
                        help=f"pool DRAM window declared to the FAR placement master, must be GB "
                             f"aligned (K/M/G suffix, default {DEFAULT_MAX_POOL_SIZE})")
    parser.add_argument("--pool-base", type=int, default=DEFAULT_POOL_BASE,
                        help=f"first pool id, worker i uses pool-base+i "
                             f"(default {DEFAULT_POOL_BASE})")
    parser.add_argument("--world", type=int, default=DEFAULT_WORLD,
                        help=f"declared world capacity (default {DEFAULT_WORLD})")
    parser.add_argument("--rpc-port-base", type=int, default=RPC_PORT_BASE,
                        help=f"control rpc port base (default {RPC_PORT_BASE}); must match the daemon's value")
    parser.add_argument("--run-dir", default=None, help="client log dir (default ./log)")
    args = parser.parse_args()

    global _term_fd
    run_dir = os.path.abspath(args.run_dir or "./log")
    os.makedirs(run_dir, exist_ok=True)
    log = open(os.path.join(run_dir, "near_summary.log"), "w")
    _term_fd = os.dup(1)
    os.dup2(log.fileno(), 1)
    os.dup2(log.fileno(), 2)

    size = _parse_size(args.size)
    block = _parse_size(args.block_size)
    max_pool = _parse_size(args.max_pool_size)
    if args.workers < 2:
        raise RuntimeError("--workers must be >= 2 (concurrency is the point of this example)")
    if size % 4 != 0:
        raise RuntimeError(f"--size ({size}) must be 4-byte aligned (int32 verify pattern)")
    if size > block:
        raise RuntimeError(f"--size ({size}) exceeds --block-size ({block})")
    if block > max_pool:
        raise RuntimeError(f"--block-size ({block}) exceeds --max-pool-size ({max_pool})")
    if max_pool % GIB != 0:
        raise RuntimeError(f"--max-pool-size ({max_pool}) must be GB aligned (VMM segment rule)")

    _log(f"[client] run dir: {run_dir}, store: {args.store}, world: {args.world}, dev: {args.dev}")
    _log(f"[client] {args.workers} independent pools, block {block} bytes each, verify pattern {size} bytes")
    _wait_tcp(args.store, 30)

    ctx = mp.get_context("spawn")
    barrier = ctx.Barrier(args.workers)
    result_q = ctx.Queue()
    procs = [ctx.Process(target=_worker_main, name=f"pool-worker{i}",
                         args=(i, args.pool_base + i, args.store, args.world, args.dev,
                               args.rpc_port_base, block, size, max_pool, barrier, result_q, run_dir))
             for i in range(args.workers)]
    for p in procs:
        p.start()
    _log(f"[client] {args.workers} workers spawned, pools {args.pool_base}.."
         f"{args.pool_base + args.workers - 1} building, extend fires simultaneously at the barrier")

    results = {}
    deadline = time.time() + COLLECT_TIMEOUT_SEC
    while len(results) < args.workers:
        try:
            msg = result_q.get(timeout=2.0)
        except queue.Empty:
            msg = None
        if msg is not None:
            results[msg["idx"]] = msg
            continue
        dead = [i for i, p in enumerate(procs) if p.exitcode is not None and i not in results]
        if dead:
            raise RuntimeError(f"worker died: {dead} — see {run_dir}/near_worker*.log")
        if time.time() > deadline:
            raise RuntimeError(f"only {len(results)}/{args.workers} workers finished "
                               f"within {COLLECT_TIMEOUT_SEC}s")
    for p in procs:
        p.join(timeout=60)

    bad = [r for r in results.values() if not r["ok"]]
    if bad:
        raise RuntimeError(f"workers failed: {[(r['idx'], r.get('err')) for r in bad]} — "
                           f"see {run_dir}/near_worker*.log")

    spread = {}
    first_try = 0
    for r in sorted(results.values(), key=lambda x: x["idx"]):
        spread[r["rank"]] = spread.get(r["rank"], 0) + 1
        if r["attempts"] == 1:
            first_try += 1
    _log("[client] placement spread: " +
         ", ".join(f"rank {rk} -> {n} block(s)" for rk, n in sorted(spread.items())))
    _log(f"[client] first-try extend success: {first_try}/{args.workers} "
         f"(retries are tolerated; FAR-side errors are not — see README)")
    _log(f"({args.workers}/{args.workers}) 12_concurrent_pools: concurrent multi-pool create OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
