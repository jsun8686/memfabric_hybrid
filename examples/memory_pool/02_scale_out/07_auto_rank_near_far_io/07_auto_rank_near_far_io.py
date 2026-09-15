#!/usr/bin/env python3
# coding=utf-8
# Copyright: (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT ANY KIND OF EITHER EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FITNESS
# FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.
"""07: auto-rank NEAR/FAR scale-out IO matrix over device RDMA.

Topology (manual per-node launch):
- FAR nodes: one command per node; the parent probes hccn_tool for NPU cards with
  LINK UP device-RDMA and spawns ONE resident fardev child per such card.
  The store URL must point at a FAR node, so the store server (and the master
  service) lands on the FAR side; FAR daemons keep running across NEAR runs.
- NEAR nodes: one command per node; W independent nearworker children (auto-rank,
  one handle each, round-robin over the LINK UP cards) run a multi-granularity
  copy matrix against a FAR-contributed remote HBM block and exit on completion.

Selection rule: only NPU devices whose hccn link is UP participate, on both sides.
"""
import argparse
import glob
import json
import os
import shutil
import socket
import subprocess
import sys
import time

import memfabric_hybrid as mf
from memfabric_hybrid import ralloc

DEFAULT_WORLD = 512          # declared world capacity, actual members join dynamically
NIC_PORT_BASE = 10005        # data-plane nic port base (set_nic); NOT the control rpc port
RPC_PORT_BASE = 11100        # control rpc port = base + rankId (smem_ralloc_def.h default);
                             # node-local: stale same-rank processes from a previous session
                             # on the same node squat exactly this port -> bind failure
HBM_WINDOW = 1 << 30         # 1GB HBM window slot per pool (device media)
DEFAULT_SIZES = "64K,256K,1M,4M,16M"
DEFAULT_MB_PER_SIZE = 256    # one-way traffic per size per worker
EXTEND_RETRY_SEC = 5
READY_TIMEOUT_SEC = 180      # per child init gate (ralloc init timeout defaults to 120s)
WORKER_TIMEOUT_SEC = 1800    # per worker completion gate
GIB = 1 << 30

DATA_OP = ralloc.RallocDataOpType.SDMA | ralloc.RallocDataOpType.DEVICE_RDMA
MEM_TYPE = ralloc.RallocMemType.DEVICE


def _log(msg):
    print(msg, flush=True)


# ------------------------------------------------------------------ infra ----

def _nic_ip():
    """Data-plane NIC ip: env override wins, else hostname, else UDP-connect trick."""
    ip = os.environ.get("MF_TEST_NIC_IP")
    if ip:
        return ip
    try:
        return socket.gethostbyname(socket.gethostname())
    except OSError:
        pass
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("8.8.8.8", 80))  # route lookup only, no packet is sent
            return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"


def _wait_tcp(url, timeout_sec):
    host, port = url.split("://", 1)[1].rsplit(":", 1)
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        with socket.socket() as s:
            if s.connect_ex((host, int(port))) == 0:
                return
        time.sleep(0.5)
    raise RuntimeError(f"endpoint not reachable within {timeout_sec}s: {url}")


def _store_precheck(store_url):
    """Fresh-session guard on the store-host node: the store port must be FREE before we
    spawn children. An already-accepting store is a leftover from a previous run — silently
    attaching to it would reuse its rank counter and its stale master/placement state."""
    host, port = store_url.split("://", 1)[1].rsplit(":", 1)
    if host not in (_nic_ip(), socket.gethostname()):
        return  # the store lives on another node (multi-FAR topology) — nothing to guard
    with socket.socket() as s:
        if s.connect_ex((host, int(port))) == 0:
            raise RuntimeError(f"stale store suspected: {store_url} is already accepting on "
                               f"this node — kill the leftover process (ss -lntp | grep {port}, "
                               f"ps -ef | grep 07_auto_rank) or start with a fresh --store port")


def _no_leftover_session():
    """Refuse to run alongside leftover processes of this test on the same node: they squat
    the node-local rpc ports (11100+rank) and, worse, a dying old session can be joined
    mid-flight (cross-session race -> dead QP endpoints -> unrecoverable FAR group state)."""
    try:
        r = subprocess.run(["pgrep", "-f", os.path.basename(__file__)],
                           capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        return  # pgrep unavailable — skip silently
    pids = {int(t) for t in r.stdout.split() if t.strip().isdigit()}
    pids.discard(os.getpid())
    if pids:
        raise RuntimeError(f"leftover test processes still running on this node (pids "
                           f"{sorted(pids)}) — kill them before starting a fresh session "
                           f"(ps -ef | grep 07_auto_rank | grep -v grep)")


def _write(path, obj):
    with open(path, "w") as f:
        json.dump(obj, f)


def _wait_file(path, timeout_sec):
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        if os.path.exists(path):
            with open(path) as f:
                return json.load(f)
        time.sleep(1.0)
    raise RuntimeError(f"file not produced within {timeout_sec}s: {path}")


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


# --------------------------------------------------------- rdma selection ----

def _hccn(dev, sub):
    """Run hccn_tool and return stdout, or None when the call fails."""
    try:
        r = subprocess.run(["hccn_tool", "-i", str(dev), sub, "-g"],
                           capture_output=True, text=True, timeout=10)
        return r.stdout if r.returncode == 0 else None
    except (OSError, subprocess.TimeoutExpired):
        return None


def _link_up(dev):
    out = _hccn(dev, "-link")
    return out is not None and "link status: UP" in out


def _enumerate_devices():
    """NPU ids from /dev/davinciN device nodes, sorted; fallback 0..7."""
    ids = set()
    for path in glob.glob("/dev/davinci[0-9]*"):
        base = os.path.basename(path)[len("davinci"):]
        if base.isdigit():
            ids.add(int(base))
    if not ids:
        ids = set(range(8))
    return sorted(ids)


def _rdma_up_devices(explicit):
    """Devices usable as device-RDMA endpoints: LINK UP per hccn_tool; MF_TEST_RDMA_DEVS
    or --devs forces a list (every forced device is still verified UP, else abort)."""
    if explicit:
        devs = [int(t) for t in explicit.split(",") if t.strip() != ""]
    else:
        devs = [int(t) for t in os.environ.get("MF_TEST_RDMA_DEVS", "").split(",")
                if t.strip() != ""]
        if not devs:
            devs = _enumerate_devices()
    up = []
    for dev in devs:
        if _link_up(dev):
            ip = _hccn(dev, "-ip") or ""
            ip = ip.strip().replace("\n", " ")
            _log(f"[rdma] npu {dev}: LINK UP ({ip})")
            up.append(dev)
        else:
            msg = f"[rdma] npu {dev}: link not UP, excluded"
            if explicit:
                raise RuntimeError(msg + " (explicitly forced via --devs)")
            _log(msg)
    if not up:
        raise RuntimeError("no LINK UP device-RDMA NPU found (hccn_tool -i N -link -g); "
                           "force via --devs or MF_TEST_RDMA_DEVS")
    return up


# ---------------------------------------------------------------- children ----

def _fardev_main(dev, run_dir, store_url, world, rpc_base):
    mf.set_log_level(1)  # INFO and up so operators can trace placement/store markers
    assert mf.initialize() == 0, "mf.initialize failed"
    ralloc_inited = False
    try:
        cfg = ralloc.RallocConfig()
        cfg.auto_ranking = True
        cfg.role = ralloc.RallocRole.FAR
        cfg.start_store = True  # store url lives on a FAR node; RacingForStoreServer
        cfg.dynamic_world_size = True  # NEAR workers join and leave while FARs stay
        cfg.rpc_port_base = rpc_base
        cfg.set_nic(f"tcp://{_nic_ip()}:{NIC_PORT_BASE}")
        assert ralloc.initialize(store_url, world, dev, cfg) == 0, "ralloc.initialize failed"
        ralloc_inited = True
        rank = ralloc.get_rank_id()
        _log(f"[fardev npu {dev}] auto-ranked as rank {rank}, contributor ready")
        _write(os.path.join(run_dir, f"far_dev{dev}_ready.json"), {"dev": dev, "rank": rank})

        try:
            _wait_file(os.path.join(run_dir, "shutdown.json"), 7 * 24 * 3600)
        except KeyboardInterrupt:  # Ctrl+C hits the whole process group; exit cleanly
            pass
        _log(f"[fardev npu {dev}] shutdown requested, exiting")
    finally:
        if ralloc_inited:
            ralloc.uninitialize(0)
        mf.uninitialize()


def _nearworker_main(dev, idx, run_dir, store_url, world, sizes_str, mb_per_size, remote_mb, rpc_base):
    sizes = [_parse_size(t) for t in sizes_str.split(",") if t.strip() != ""]
    if not sizes:
        raise RuntimeError("empty size list")
    remote_bytes = remote_mb << 20
    mf.set_log_level(1)
    assert mf.initialize() == 0, "mf.initialize failed"
    ralloc_inited = False
    try:
        import torch
        import torch_npu  # noqa: F401  registers the NPU backend

        cfg = ralloc.RallocConfig()
        cfg.auto_ranking = True
        cfg.role = ralloc.RallocRole.NEAR
        cfg.start_store = False  # the store lives on the FAR side
        cfg.dynamic_world_size = True
        cfg.rpc_port_base = rpc_base
        cfg.set_nic(f"tcp://{_nic_ip()}:{NIC_PORT_BASE}")
        assert ralloc.initialize(store_url, world, dev, cfg) == 0, "ralloc.initialize failed"
        ralloc_inited = True
        rank = ralloc.get_rank_id()

        handle = ralloc.create(id=0, max_dram_size=0, max_hbm_size=HBM_WINDOW, data_op_type=DATA_OP)

        # FAR contributors may still be registering; placement fails until then
        deadline = time.time() + 300
        while True:
            ret, info = handle.extend_remote_mem(MEM_TYPE, remote_bytes)
            if ret == 0 and info.get("gva"):
                break
            if time.time() > deadline:
                raise RuntimeError(f"no FAR candidate within 300s: {ret} {info}")
            time.sleep(EXTEND_RETRY_SEC)
        gva = info["gva"]
        far_rank = info["rank_id"]
        # drain the sticky retry error from the loop above (04/05 pattern)
        mf.get_and_clear_last_err_msg()
        _log(f"[near w{idx} rank {rank}] remote block from FAR rank {far_rank} "
             f"(gva=0x{gva:x}, npu {dev})")

        device = f"npu:{dev}"
        results = {}
        for size in sizes:
            if size > remote_bytes:
                raise RuntimeError(f"size {size} exceeds remote block {remote_bytes}")
            src = torch.arange(size // 4, dtype=torch.int32, device=device).contiguous()
            dst = torch.empty_like(src)
            # untimed correctness probe for this granularity
            assert handle.copy_data(src.data_ptr(), gva, size, 0) == 0, "probe H2G"
            assert handle.copy_data(gva, dst.data_ptr(), size, 0) == 0, "probe G2H"
            assert handle.wait() == 0, "probe wait"
            assert torch.equal(dst, src), "probe round-trip mismatch"

            slots = remote_bytes // size
            blocks = (mb_per_size << 20) // size
            t0 = time.perf_counter()
            for i in range(blocks):
                off = gva + (i % slots) * size
                assert handle.copy_data(src.data_ptr(), off, size, 0) == 0, "H2G"
                assert handle.copy_data(off, dst.data_ptr(), size, 0) == 0, "G2H"
            assert handle.wait() == 0, "matrix wait"
            dt = time.perf_counter() - t0
            one_way = blocks * size
            gbps = one_way / dt / GIB
            us = dt / blocks * 1e6
            results[str(size)] = {"blocks": blocks, "gbps": round(gbps, 2),
                                  "us_per_block": round(us, 2)}
            _log(f"[near w{idx}] size {size}: {blocks} blocks, {gbps:.2f} GB/s, "
                 f"{us:.2f} us/block (round-trip)")

        handle.destroy()
        mf.get_and_clear_last_err_msg()
        assert mf.get_last_err_msg() == "", mf.get_last_err_msg()
        _write(os.path.join(run_dir, f"near_w{idx}_done.json"),
               {"idx": idx, "rank": rank, "dev": dev, "far_rank": far_rank, "results": results})
        _log(f"[near w{idx}] worker finished, exiting")
    finally:
        if ralloc_inited:
            ralloc.uninitialize(0)
        mf.uninitialize()


# ----------------------------------------------------------------- parents ----

def _spawn(role_argv, run_dir, log_name):
    log_path = os.path.join(run_dir, log_name)
    f = open(log_path, "w")
    cmd = [sys.executable, os.path.abspath(__file__)] + role_argv
    p = subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT)
    return p, f


def _far_parent(args, run_dir):
    _store_precheck(args.store)  # abort early on a leftover store from a previous run
    devs = _rdma_up_devices(args.devs)
    procs, files = {}, {}
    try:
        for dev in devs:
            procs[dev], files[dev] = _spawn(
                ["fardev", str(dev), run_dir, args.store, str(args.world), str(args.rpc_port_base)],
                run_dir, f"far_dev{dev}.log")
        rank_map = {}
        for dev in devs:
            info = _wait_file(os.path.join(run_dir, f"far_dev{dev}_ready.json"), READY_TIMEOUT_SEC)
            rank_map[dev] = info["rank"]
        _log("[far] resident contributors: " +
             ", ".join(f"npu {d} -> rank {r}" for d, r in sorted(rank_map.items())))
        _log("[far] daemons running — stop with: touch " +
             os.path.join(run_dir, "shutdown.json") + "  (or Ctrl+C)")

        shutdown = os.path.join(run_dir, "shutdown.json")
        try:
            while not os.path.exists(shutdown):
                for dev, p in procs.items():
                    if p.poll() is not None:
                        _log(f"[far] WARN: fardev npu {dev} exited (code {p.returncode})")
                time.sleep(5)
        except KeyboardInterrupt:
            _log("[far] interrupted, stopping children")
        _write(shutdown, {})
        for p in procs.values():
            if p.poll() is None:
                p.wait(timeout=90)
        bad = [d for d, p in procs.items() if p.returncode not in (0, None)]
        if bad:
            raise RuntimeError(f"fardev failed on npu {bad} — see {run_dir}")
        _log("[far] all contributors exited cleanly")
    finally:
        for f in files.values():
            f.close()


def _near_parent(args, run_dir):
    _wait_tcp(args.store, 30)  # the FAR-hosted store must accept first
    devs = _rdma_up_devices(args.devs)
    sizes = [_parse_size(t) for t in args.sizes.split(",") if t.strip() != ""]
    if not sizes:
        raise RuntimeError("empty --sizes")
    workers = max(1, args.workers)
    remote_bytes = args.remote_mb << 20

    procs, files = {}, {}
    try:
        for idx in range(workers):
            dev = devs[idx % len(devs)]
            procs[idx], files[idx] = _spawn(
                ["nearworker", str(dev), str(idx), run_dir, args.store, str(args.world),
                 args.sizes, str(args.mb_per_size), str(args.remote_mb), str(args.rpc_port_base)],
                run_dir, f"near_w{idx}.log")
            _log(f"[near] worker {idx} on npu {dev}")

        results = {}
        for idx in range(workers):
            done_path = os.path.join(run_dir, f"near_w{idx}_done.json")
            deadline = time.time() + WORKER_TIMEOUT_SEC
            while True:
                if os.path.exists(done_path):
                    with open(done_path) as f:
                        results[idx] = json.load(f)
                    break
                rc = procs[idx].poll()
                if rc is not None:
                    raise RuntimeError(f"worker {idx} exited (code {rc}) before finishing "
                                       f"— see {run_dir}/near_w{idx}.log")
                if time.time() > deadline:
                    raise RuntimeError(f"worker {idx} not done within {WORKER_TIMEOUT_SEC}s")
                time.sleep(2)
        for idx in range(workers):
            procs[idx].wait(timeout=120)
            if procs[idx].returncode != 0:
                raise RuntimeError(f"worker {idx} failed: {procs[idx].returncode} — see {run_dir}")

        _log("")
        _log(f"{'size':>10s} " + " ".join(f"{'w' + str(i):>8s}" for i in range(workers)) +
             f" {'min':>8s} {'avg':>8s} {'max':>8s}   GB/s (one-way, round-trip timed)")
        for size in sizes:
            key = str(size)
            vals = [results[i]["results"][key]["gbps"] for i in range(workers)]
            _log(f"{key:>10s} " + " ".join(f"{v:8.2f}" for v in vals) +
                 f" {min(vals):8.2f} {sum(vals) / len(vals):8.2f} {max(vals):8.2f}")
        far_ranks = sorted({results[i]["far_rank"] for i in range(workers)})
        _log(f"(workers={workers}, sizes={args.sizes}, FAR ranks hit={far_ranks})")
        print(f"({workers}/{workers}) 07_auto_rank_near_far_io: near IO matrix OK", flush=True)
    finally:
        for p in procs.values():
            if p.poll() is None:
                p.terminate()  # reap sibling workers on failure / Ctrl+C — no orphans left behind
        for p in procs.values():
            try:
                p.wait(timeout=15)
            except subprocess.TimeoutExpired:
                p.kill()
        for f in files.values():
            f.close()


# --------------------------------------------------------------------- main ----

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("role", choices=["far", "near"], help="node role to run")
    parser.add_argument("--store", required=True,
                        help="store url, MUST point at a FAR node, e.g. tcp://10.0.0.1:8587")
    parser.add_argument("--world", type=int, default=DEFAULT_WORLD,
                        help=f"declared world capacity (default {DEFAULT_WORLD})")
    parser.add_argument("--devs", default=None,
                        help="comma list of NPU ids to force (default: auto-detect LINK UP)")
    parser.add_argument("--workers", type=int, default=4,
                        help="NEAR: concurrent worker processes (default 4)")
    parser.add_argument("--sizes", default=DEFAULT_SIZES,
                        help=f"NEAR: copy granularity sweep (default {DEFAULT_SIZES})")
    parser.add_argument("--mb-per-size", type=int, default=DEFAULT_MB_PER_SIZE,
                        help=f"NEAR: one-way MB per size per worker (default {DEFAULT_MB_PER_SIZE})")
    parser.add_argument("--remote-mb", type=int, default=64,
                        help="NEAR: remote block size in MB (default 64)")
    parser.add_argument("--rpc-port-base", type=int, default=RPC_PORT_BASE,
                        help=f"control rpc port base (port = base + rank_id, default {RPC_PORT_BASE}); "
                             f"use to dodge stale same-rank processes on shared nodes — keep one "
                             f"value for the whole session")
    parser.add_argument("--run-dir", default=None, help="marker/log dir (default ./log)")
    args = parser.parse_args()

    run_dir = os.path.abspath(args.run_dir or "./log")
    os.makedirs(run_dir, exist_ok=True)
    _log(f"[{args.role}] run dir: {run_dir}, store: {args.store}, world: {args.world}")
    _no_leftover_session()  # both roles: refuse to coexist with a previous session here

    if args.role == "far":
        _far_parent(args, run_dir)
        return 0
    try:
        _near_parent(args, run_dir)
    except Exception:
        print(f"[near] FAIL — logs kept in {run_dir}", flush=True)
        raise
    if not os.environ.get("MF_KEEP_LOGS"):
        shutil.rmtree(run_dir, ignore_errors=True)  # keep logs on failure / MF_KEEP_LOGS=1
    return 0


if __name__ == "__main__":
    child_argv = sys.argv[1:]
    if child_argv and child_argv[0] in ("fardev", "nearworker"):  # child entry
        if child_argv[0] == "fardev":
            _fardev_main(int(child_argv[1]), child_argv[2], child_argv[3], int(child_argv[4]),
                         int(child_argv[5]))
        else:
            _nearworker_main(int(child_argv[1]), int(child_argv[2]), child_argv[3], child_argv[4],
                             int(child_argv[5]), child_argv[6], int(child_argv[7]),
                             int(child_argv[8]), int(child_argv[9]))
        sys.exit(0)
    sys.exit(main())
