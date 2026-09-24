#!/usr/bin/env python3
# coding=utf-8
# Copyright: (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.

import argparse
import multiprocessing as mp
import os
import queue
import signal
import socket
import sys
import time

import memfabric_hybrid as mf
from memfabric_hybrid import ralloc

DEFAULT_WORLD = 512          # declared world capacity, actual members join dynamically
NIC_PORT_BASE = 10010        # data-plane nic port base (set_nic); NOT the control rpc port
RPC_PORT_BASE = 11105        # control rpc port = base + rankId (smem_ralloc_def.h default)
READY_TIMEOUT_SEC = 180      # contributor init gate (ralloc init timeout defaults to 120s)
JOIN_TIMEOUT_SEC = 90        # graceful stop gate before terminate()


def _log(msg):
    print(msg, flush=True)


def _contributor_main(dev, run_dir, store_url, world, rpc_base, ready_q, stop_event):
    log = open(os.path.join(run_dir, f"far_dev{dev}.log"), "w")
    os.dup2(log.fileno(), 1)
    os.dup2(log.fileno(), 2)
    mf.set_log_level(1)
    assert mf.initialize() == 0, "mf.initialize failed"
    ralloc_inited = False
    try:
        cfg = ralloc.RallocConfig()
        cfg.auto_ranking = True
        cfg.role = ralloc.RallocRole.FAR
        cfg.start_store = True
        cfg.dynamic_world_size = True
        cfg.rpc_port_base = rpc_base
        cfg.set_nic(f"tcp://{socket.gethostbyname(socket.gethostname())}:{NIC_PORT_BASE}")
        assert ralloc.initialize(store_url, world, dev, cfg) == 0, "ralloc.initialize failed"
        ralloc_inited = True
        rank = ralloc.get_rank_id()
        _log(f"[far npu {dev}] contributor ready: rank {rank} (store {store_url})")
        ready_q.put({"dev": dev, "rank": rank})

        parent_pid = os.getppid()
        while not stop_event.wait(timeout=1.0):
            if os.getppid() != parent_pid:
                _log(f"[far npu {dev}] daemon gone, exiting")
                break
    except KeyboardInterrupt:
        pass
    finally:
        if ralloc_inited:
            ralloc.uninitialize(0)
        mf.uninitialize()
    _log(f"[far npu {dev}] contributor stopped cleanly")


def _collect_ready(ready_q, procs, devs, timeout_sec):
    got = {}
    deadline = time.time() + timeout_sec
    while len(got) < len(devs):
        try:
            msg = ready_q.get(timeout=2.0)
        except queue.Empty:
            msg = None
        if msg is not None:
            got[msg["dev"]] = msg
            continue
        dead = [p.name for p, dev in zip(procs, devs) if p.exitcode is not None and dev not in got]
        if dead:
            raise RuntimeError(f"contributor died before ready: {dead} — see far_dev*.log")
        if time.time() > deadline:
            raise RuntimeError(f"only {len(got)}/{len(devs)} contributors ready within {timeout_sec}s")
    return got


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--store", required=True,
                        help="store url, MUST point at THIS FAR node, e.g. tcp://10.0.0.1:8588")
    parser.add_argument("--devs", required=True,
                        help="comma list of NPU ids to contribute, e.g. 0,1 (one child per card)")
    parser.add_argument("--world", type=int, default=DEFAULT_WORLD,
                        help=f"declared world capacity (default {DEFAULT_WORLD})")
    parser.add_argument("--rpc-port-base", type=int, default=RPC_PORT_BASE,
                        help=f"control rpc port base (port = base + rank_id, default {RPC_PORT_BASE}); "
                             f"keep one value for the whole session, clients included")
    parser.add_argument("--run-dir", default=None, help="per-contributor log dir (default ./log)")
    args = parser.parse_args()

    devs = [int(t) for t in args.devs.split(",") if t.strip() != ""]
    if not devs:
        raise RuntimeError("empty --devs")
    run_dir = os.path.abspath(args.run_dir or "./log")
    os.makedirs(run_dir, exist_ok=True)
    _log(f"[daemon] run dir: {run_dir}, store: {args.store}, world: {args.world}, devs: {devs}")

    ctx = mp.get_context("spawn")
    ready_q = ctx.Queue()
    stop_event = ctx.Event()
    procs = [ctx.Process(target=_contributor_main, name=f"far-npu{dev}",
                         args=(dev, run_dir, args.store, args.world, args.rpc_port_base, ready_q, stop_event))
             for dev in devs]
    for p in procs:
        p.start()

    def _stop_signal(_signum, _frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, _stop_signal)
    try:
        got = _collect_ready(ready_q, procs, devs, READY_TIMEOUT_SEC)
        pairs = ", ".join(f"npu{dev}->rank{got[dev]['rank']}" for dev in devs)
        _log(f"[daemon] {len(devs)} memory contributors serving ({pairs})")
        _log(f"[daemon] try the client:  python3 memfabric_client.py --store {args.store} --dev <npu_id>")
        _log(f"[daemon] per-contributor logs: {run_dir}/far_dev<N>.log")
        _log(f"[daemon] stop with Ctrl+C or:  kill -TERM {os.getpid()}")
        warned = set()
        while True:
            for p in procs:
                if p.exitcode is not None and p.name not in warned:
                    warned.add(p.name)
                    _log(f"[daemon] WARN: {p.name} exited (code {p.exitcode})")
            time.sleep(5)
    except KeyboardInterrupt:
        _log("[daemon] stop requested")
    finally:
        stop_event.set()
        for p in procs:
            p.join(timeout=JOIN_TIMEOUT_SEC)
        for p in procs:
            if p.is_alive():
                p.terminate()
        for p in procs:
            p.join(timeout=15)
        bad = [p.name for p in procs if p.exitcode not in (0, None)]
        if bad:
            raise RuntimeError(f"contributor failed: {bad} — see {run_dir}/far_dev*.log")
        _log("[daemon] all contributors stopped cleanly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
