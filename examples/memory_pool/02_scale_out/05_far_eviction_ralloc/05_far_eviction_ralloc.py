#!/usr/bin/env python3
# coding=utf-8
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.
"""R12-B fault injection: kill one FAR contributor mid-run and assert the master's
store rank-down watch evicts it within seconds, then placement re-selects the survivor.

Implicitly also covers:
- rank-watch multi-waiter: the NEAR process holds the master subscription plus TWO
  per-entry group-engine subscriptions on the same store link (pre-fix: SM_REPEAT_CALL)
- etcd variant (--store etcd): fixed-rank registration at connect (factory rankId
  passthrough) — without it the rank never registers and the rank-down never fires

Parent spawns three children (subprocess form so the victim can be SIGKILLed while
the survivors keep asserting); per-rank logs land in the run dir for post-hoc greps.
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

import torch

import memfabric_hybrid as mf
from memfabric_hybrid import ralloc

ONE_GIB = 1 << 30  # 1GB window slot per rank (VA reservation only)
LOCAL_BYTES = 32 * 1024 * 1024    # 32MB local slot commit per pool
REMOTE_BYTES = 64 * 1024 * 1024   # 64MB first remote block
REMOTE_AGAIN = 32 * 1024 * 1024   # 32MB post-eviction re-acquire
COPY_BYTES = 4 * 1024 * 1024      # 4MB int32 round-trip payload
WORLD_SIZE = 3
RANK_NEAR, RANK_FAR1, RANK_FAR2 = 0, 1, 2
NIC_PORT_BASE = 10005  # executor rpc port = base + rankId
MEM_TYPE = ralloc.RallocMemType.HOST
DATA_OP = ralloc.RallocDataOpType.HOST_RDMA

TCP_STORE_URL = "tcp://127.0.0.1:8585"
ETCD_STORE_URL = "etcd://127.0.0.1:2379"

# log markers asserted by the parent (exact smem log strings, keep in sync with src)
MARK_BECAME_LEADER = "Became leader"
MARK_RANK_DOWN = "candidate rank-down, rank: "
MARK_ALREADY_WATCHED = "already watched for rank state"  # must NOT appear (multi-waiter fix)
MARK_BACKOFF = "placement reused failed rank"            # soft: race-window dependent


def _nic_ip():
    """Data-plane NIC ip: env override wins, else the node's primary ip (like 03/04).
    gethostbyname needs a resolvable hostname (bare cluster hosts lack it), so fall
    back to the no-packet UDP-connect trick, then loopback (single-node tests)."""
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


def _tcp_up(url):
    host, port = url.split("://", 1)[1].rsplit(":", 1)
    with socket.socket() as s:
        return s.connect_ex((host, int(port))) == 0


def _wait_alive(cond, timeout_sec, tag, procs, run_dir):
    """Gate on cond() but fail fast when any watched child exits first — a dead child
    would otherwise stall the gate for the full timeout with a misleading message."""
    deadline = time.time() + timeout_sec
    while True:
        if cond():
            return
        for rank, p in procs.items():
            if p.poll() is not None:
                raise RuntimeError(
                    f"{tag}: child rank {rank} exited (code {p.returncode}) — see {run_dir}")
        if time.time() > deadline:
            raise RuntimeError(f"{tag}: condition not met within {timeout_sec}s")
        time.sleep(2)


def _wait_file_alive(path, timeout_sec, tag, procs, run_dir):
    deadline = time.time() + timeout_sec
    while True:
        if os.path.exists(path):
            with open(path) as f:
                return json.load(f)
        for rank, p in procs.items():
            if p.poll() is not None:
                raise RuntimeError(
                    f"{tag}: child rank {rank} exited (code {p.returncode}) — see {run_dir}")
        if time.time() > deadline:
            raise RuntimeError(f"{tag}: file not produced within {timeout_sec}s: {path}")
        time.sleep(1.0)


def _round_trip(handle, gva):
    src = torch.arange(COPY_BYTES // 4, dtype=torch.int32).contiguous()
    assert handle.copy_data(src.data_ptr(), gva, COPY_BYTES, 0) == 0, "H2G into far slot"
    got = torch.empty(COPY_BYTES // 4, dtype=torch.int32)
    assert handle.copy_data(gva, got.data_ptr(), COPY_BYTES, 0) == 0, "G2H from far slot"
    assert torch.equal(got, src), "round-trip mismatch"


def _retry_extend_remote(handle, size, timeout_sec, tag):
    """Placement fails (nonzero ret) while FARs are still registering / after the kill;
    retry until success. Sticky last-error is drained by the caller afterwards."""
    deadline = time.time() + timeout_sec
    while True:
        ret, info = handle.extend_remote_mem(MEM_TYPE, size)
        if ret == 0 and info.get("gva"):
            return info
        if time.time() > deadline:
            raise RuntimeError(f"{tag}: extend_remote_mem failed within {timeout_sec}s: {ret} {info}")
        time.sleep(5)


def _retry_assert(cond, timeout_sec, tag):
    deadline = time.time() + timeout_sec
    while True:
        if cond():
            return
        if time.time() > deadline:
            raise RuntimeError(f"{tag}: condition not met within {timeout_sec}s")
        time.sleep(2)


# ---------------------------------------------------------------- children ----

def _near_main(store_url, run_dir):
    mf.set_log_level(1)  # INFO and up: the parent asserts on INFO/WARN log markers (3 = ERROR only)
    assert mf.initialize() == 0, "mf.initialize failed"
    ralloc_inited = False
    try:
        cfg = ralloc.RallocConfig()
        cfg.rank_id = RANK_NEAR
        cfg.auto_ranking = False
        cfg.role = ralloc.RallocRole.NEAR
        cfg.start_store = True  # NEAR hosts the store; the master service activates here
        cfg.set_nic(f"tcp://{_nic_ip()}:{NIC_PORT_BASE}")
        assert ralloc.initialize(store_url, WORLD_SIZE, 0, cfg) == 0, "ralloc.initialize failed"
        ralloc_inited = True

        # two pools -> two group engines + the master subscription share ONE store link
        pools = []
        for pool_id in (0, 1):
            h = ralloc.create(id=pool_id, max_dram_size=ONE_GIB, max_hbm_size=0, data_op_type=DATA_OP)
            ret, info = h.extend_local_mem(MEM_TYPE, LOCAL_BYTES)
            assert ret == 0 and info["gva"] != 0, f"pool {pool_id} extend_local_mem: {ret} {info}"
            pools.append(h)

        info = _retry_extend_remote(pools[0], REMOTE_BYTES, 120, "initial acquire")
        acquired = info["rank_id"]
        assert acquired in (RANK_FAR1, RANK_FAR2), f"unexpected contributor rank: {acquired}"
        _round_trip(pools[0], info["gva"])
        print(f"[near] acquired from rank {acquired}, round-trip OK", flush=True)
        _write(os.path.join(run_dir, "near_acquired.json"), {"rank": acquired})

        killed = _wait_file(os.path.join(run_dir, "killed.json"), 300)["rank"]
        assert killed == acquired, f"parent killed {killed}, NEAR acquired from {acquired}"

        # the master's rank-down watch evicts the victim within seconds; the survivor
        # (or the same-node backoff inside extend_remote_mem) then serves this request
        info2 = _retry_extend_remote(pools[0], REMOTE_AGAIN, 90, "post-eviction acquire")
        survivor = RANK_FAR1 + RANK_FAR2 - killed
        assert info2["rank_id"] == survivor, \
            f"placement returned dead rank {info2['rank_id']}, expected {survivor}"
        _round_trip(pools[0], info2["gva"])
        print(f"[near] re-acquired from rank {info2['rank_id']} after eviction, round-trip OK", flush=True)

        # dead member leaves the group once the engine digests LINK_DOWN (R13, async)
        _retry_assert(lambda: sorted(pools[0].get_group_ranks()) == [RANK_NEAR, survivor],
                      30, "group ranks shrink to survivor")

        mf.get_and_clear_last_err_msg()  # drain sticky retry errors (04 pattern)
        for h in reversed(pools):
            h.destroy()
        assert mf.get_last_err_msg() == "", mf.get_last_err_msg()

        # hold this process (it hosts the tcp store) alive until the survivor FAR has
        # finished its clean uninitialize, which needs store access — exiting first
        # would trap the FAR in an unbounded CAS retry against a dead store
        _write(os.path.join(run_dir, "near_finished.json"), {})
        _wait_file(os.path.join(run_dir, "near_exit.json"), 300)
    finally:
        if ralloc_inited:
            ralloc.uninitialize(0)
        mf.uninitialize()


def _far_main(rank, store_url, run_dir):
    mf.set_log_level(1)  # INFO and up: the parent asserts on INFO/WARN log markers (3 = ERROR only)
    assert mf.initialize() == 0, "mf.initialize failed"
    ralloc_inited = False
    try:
        if store_url.startswith("tcp://"):
            _wait_tcp(store_url, 60)  # the NEAR-hosted store must accept first
        cfg = ralloc.RallocConfig()
        cfg.rank_id = rank
        cfg.auto_ranking = False
        cfg.role = ralloc.RallocRole.FAR
        cfg.start_store = False
        cfg.set_nic(f"tcp://{_nic_ip()}:{NIC_PORT_BASE}")
        deadline = time.time() + 60
        while True:
            if ralloc.initialize(store_url, WORLD_SIZE, 0, cfg) == 0:
                break
            if time.time() > deadline:
                raise RuntimeError(f"rank {rank}: ralloc.initialize failed within 60s")
            time.sleep(2)
        ralloc_inited = True
        print(f"[far {rank}] contributor ready", flush=True)
        _write(os.path.join(run_dir, f"far{rank}_ready.json"), {})

        _wait_file(os.path.join(run_dir, "shutdown.json"), 600)  # exits early if killed
    finally:
        if ralloc_inited:
            ralloc.uninitialize(0)
        mf.uninitialize()


# ----------------------------------------------------------------- parent ----

def _spawn(role, run_dir, store, store_url, log_name):
    log_path = os.path.join(run_dir, log_name)
    f = open(log_path, "w")
    cmd = [sys.executable, os.path.abspath(__file__), role, run_dir, store]
    if store == "etcd":
        cmd.append(store_url)
    p = subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT)
    return p, f


def _grep(path, needle):
    if not os.path.exists(path):
        return False
    with open(path, errors="replace") as f:
        return needle in f.read()


def _parent(store, etcd_url, run_dir):
    store_url = etcd_url if store == "etcd" else TCP_STORE_URL
    if store == "etcd":
        _wait_tcp(store_url, 30)  # etcd service is a hard prerequisite

    procs, files = {}, {}
    try:
        procs[RANK_NEAR], files[RANK_NEAR] = _spawn("near", run_dir, store, store_url, "rank0.log")
        rank0_log = os.path.join(run_dir, "rank0.log")
        near_only = {RANK_NEAR: procs[RANK_NEAR]}
        if store == "tcp":
            _wait_alive(lambda: _tcp_up(store_url), 60, "store up", near_only, run_dir)
        else:
            # gate the followers on a DETERMINISTIC first leader/master (election is
            # first-come-first-served with random backoff)
            _wait_alive(lambda: _grep(rank0_log, MARK_BECAME_LEADER), 60, "rank0 becomes leader",
                        near_only, run_dir)

        for rank, role, log in ((RANK_FAR1, "far1", "rank1.log"), (RANK_FAR2, "far2", "rank2.log")):
            procs[rank], files[rank] = _spawn(role, run_dir, store, store_url, log)
            _wait_file_alive(os.path.join(run_dir, f"far{rank}_ready.json"), 90, f"far{rank} ready",
                             dict(procs), run_dir)

        victim = _wait_file_alive(os.path.join(run_dir, "near_acquired.json"), 180, "NEAR acquisition",
                                  dict(procs), run_dir)["rank"]
        print(f"[parent] killing FAR rank {victim} (SIGKILL, mid-run)", flush=True)
        procs[victim].kill()  # true crash: no cleanup, the store link breaks abruptly
        procs[victim].wait(timeout=30)
        _write(os.path.join(run_dir, "killed.json"), {"rank": victim})

        # teardown ordering: the survivor FAR must uninitialize while the store is still
        # alive, so NEAR (the tcp store host) signals completion and WAITS before exiting
        survivor = RANK_FAR1 + RANK_FAR2 - victim
        _wait_file_alive(os.path.join(run_dir, "near_finished.json"), 300, "NEAR assertions done",
                         {RANK_NEAR: procs[RANK_NEAR], survivor: procs[survivor]}, run_dir)
        _write(os.path.join(run_dir, "shutdown.json"), {})
        procs[survivor].wait(timeout=90)
        assert procs[survivor].returncode == 0, f"survivor FAR failed: {procs[survivor].returncode}"

        _write(os.path.join(run_dir, "near_exit.json"), {})
        procs[RANK_NEAR].wait(timeout=90)
        assert procs[RANK_NEAR].returncode == 0, \
            f"NEAR failed: exitcode {procs[RANK_NEAR].returncode} (see {run_dir})"

        # ---- log assertions ----
        rank_down = _grep(rank0_log, f"{MARK_RANK_DOWN}{victim} ")
        assert rank_down, f"master never saw rank-down for rank {victim} (see {run_dir})"
        for rank in (RANK_NEAR, RANK_FAR1, RANK_FAR2):
            assert not _grep(os.path.join(run_dir, f"rank{rank}.log"), MARK_ALREADY_WATCHED), \
                f"rank {rank}: stale single-waiter rank-watch hit (see {run_dir})"
        backoff_hit = any(
            _grep(os.path.join(run_dir, f"rank{r}.log"), MARK_BACKOFF) for r in (RANK_NEAR, RANK_FAR1, RANK_FAR2))

        print(f"(1/1) 05_far_eviction [{store}]: eviction + re-placement OK "
              f"(victim rank {victim}, survivor rank {survivor}, "
              f"backoff path {'exercised' if backoff_hit else 'not hit (race-window)'})", flush=True)
        return 0
    finally:
        for f in files.values():
            f.close()


def main():
    args = sys.argv[1:]
    if args and args[0] in ("near", "far1", "far2"):  # child entry
        role, run_dir, store = args[0], args[1], args[2]
        etcd_url = args[3] if len(args) > 3 else ETCD_STORE_URL
        store_url = etcd_url if store == "etcd" else TCP_STORE_URL
        if role == "near":
            _near_main(store_url, run_dir)
        else:
            _far_main(RANK_FAR1 if role == "far1" else RANK_FAR2, store_url, run_dir)
        return

    store = args[0].lower() if args and args[0].lower() in ("tcp", "etcd") else "tcp"
    etcd_url = args[1] if len(args) > 1 else ETCD_STORE_URL
    import os
    run_dir = os.path.abspath("./log")
    os.makedirs(run_dir, exist_ok=True)
    print(f"[parent] run dir: {run_dir} (store={store}, etcd={etcd_url if store == 'etcd' else 'n/a'})",
          flush=True)
    try:
        rc = _parent(store, etcd_url, run_dir)
    except Exception as e:
        print(f"[parent] FAIL: {e} — logs kept in {run_dir}", flush=True)
        raise
    if rc == 0 and not os.environ.get("MF_KEEP_LOGS"):
        shutil.rmtree(run_dir, ignore_errors=True)  # keep logs only on failure (MF_KEEP_LOGS=1 keeps them)


if __name__ == "__main__":
    main()
