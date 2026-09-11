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
"""R12-A fault injection (etcd HA store): kill the store-leader host mid-run and
assert the full failover chain — lease expiry -> re-election -> leader promotion
handler -> master re-activation -> MASTER key re-publish -> FAR re-discovery ->
placement recovery on the surviving FAR.

Topology (single node, three processes): rank0 = FAR + store election participant
(deterministic FIRST leader: spawned alone until "Became leader"), rank1 = NEAR,
rank2 = FAR + store election participant. After the kill the next leader is whoever
grabs the etcd lock first (rank1 or rank2) — the assertion is rank-agnostic.

Requires a running etcd; see README for the bootstrap command.
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
LOCAL_BYTES = 32 * 1024 * 1024    # 32MB local slot commit
REMOTE_BYTES = 64 * 1024 * 1024   # 64MB first remote block
REMOTE_AGAIN = 32 * 1024 * 1024   # 32MB post-failover re-acquire
COPY_BYTES = 4 * 1024 * 1024      # 4MB int32 round-trip payload
WORLD_SIZE = 3
RANK_FAR_A, RANK_NEAR, RANK_FAR_B = 0, 1, 2  # killed master host / requester / survivor
NIC_PORT_BASE = 10005
MEM_TYPE = ralloc.RallocMemType.HOST
DATA_OP = ralloc.RallocDataOpType.HOST_RDMA

ETCD_STORE_URL = "etcd://127.0.0.1:2379"

# log markers asserted by the parent (exact smem log strings, keep in sync with src)
MARK_BECAME_LEADER = "Became leader"                    # first-leader gate (rank0)
MARK_PROMOTION = "Firing leader promotion handler"      # re-election winner (rank1 or rank2)
MARK_ENDPOINT_CHANGED = "master endpoint changed"
MARK_ENDPOINT_REFRESHED = "master endpoint refreshed"
MARK_ALREADY_WATCHED = "already watched for rank state"  # must NOT appear

RECOVERY_TIMEOUT_SEC = 120  # lease 5s + health check 4s + election backoff + FAR re-register


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
    """Fails (nonzero ret) while FARs register, then during the leaderless window
    after the kill (no master / store ops fail); retry until success."""
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
        cfg.start_store = False  # store leadership is decided by the HA election
        cfg.set_nic(f"tcp://{_nic_ip()}:{NIC_PORT_BASE}")
        assert ralloc.initialize(store_url, WORLD_SIZE, 0, cfg) == 0, "ralloc.initialize failed"
        ralloc_inited = True

        handle = ralloc.create(id=0, max_dram_size=ONE_GIB, max_hbm_size=0, data_op_type=DATA_OP)
        ret, info = handle.extend_local_mem(MEM_TYPE, LOCAL_BYTES)
        assert ret == 0 and info["gva"] != 0, f"extend_local_mem: {ret} {info}"

        info = _retry_extend_remote(handle, REMOTE_BYTES, 120, "initial acquire")
        acquired = info["rank_id"]
        assert acquired in (RANK_FAR_A, RANK_FAR_B), f"unexpected contributor rank: {acquired}"
        _round_trip(handle, info["gva"])
        print(f"[near] acquired from rank {acquired}, round-trip OK", flush=True)
        _write(os.path.join(run_dir, "near_acquired.json"), {"rank": acquired})

        killed = _wait_file(os.path.join(run_dir, "killed.json"), 300)["rank"]
        assert killed == RANK_FAR_A, f"parent killed {killed}, expected the master host {RANK_FAR_A}"

        # full failover chain must converge well before RECOVERY_TIMEOUT_SEC
        info2 = _retry_extend_remote(handle, REMOTE_AGAIN, RECOVERY_TIMEOUT_SEC, "post-failover acquire")
        assert info2["rank_id"] == RANK_FAR_B, \
            f"placement returned rank {info2['rank_id']} after failover, expected survivor {RANK_FAR_B}"
        _round_trip(handle, info2["gva"])
        print(f"[near] re-acquired from rank {RANK_FAR_B} after master failover, round-trip OK", flush=True)

        # if the first block lived on the killed host, the dead member must leave the group
        _retry_assert(lambda: sorted(handle.get_group_ranks()) == [RANK_NEAR, RANK_FAR_B],
                      30, "group ranks settle to [NEAR, survivor FAR]")

        mf.get_and_clear_last_err_msg()  # drain sticky retry errors (04 pattern)
        handle.destroy()
        assert mf.get_last_err_msg() == "", mf.get_last_err_msg()

        # hold this process alive until the survivor FAR has finished its clean
        # uninitialize — after HA re-election NEAR may itself host the store, and
        # exiting first would trap the FAR in an unbounded CAS retry on a dead store
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
        cfg = ralloc.RallocConfig()
        cfg.rank_id = rank
        cfg.auto_ranking = False
        cfg.role = ralloc.RallocRole.FAR
        cfg.start_store = True  # both FAR processes are election participants
        cfg.set_nic(f"tcp://{_nic_ip()}:{NIC_PORT_BASE}")
        deadline = time.time() + 60
        while True:
            if ralloc.initialize(store_url, WORLD_SIZE, 0, cfg) == 0:
                break
            if time.time() > deadline:
                raise RuntimeError(f"rank {rank}: ralloc.initialize failed within 60s")
            time.sleep(2)
        ralloc_inited = True
        print(f"[far {rank}] contributor ready (election participant)", flush=True)
        _write(os.path.join(run_dir, f"far{rank}_ready.json"), {})

        _wait_file(os.path.join(run_dir, "shutdown.json"), 600)  # exits early if killed
    finally:
        if ralloc_inited:
            ralloc.uninitialize(0)
        mf.uninitialize()


# ----------------------------------------------------------------- parent ----

def _spawn(role, run_dir, store_url, log_name):
    log_path = os.path.join(run_dir, log_name)
    f = open(log_path, "w")
    cmd = [sys.executable, os.path.abspath(__file__), role, run_dir, store_url]
    p = subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT)
    return p, f


def _grep(path, needle):
    if not os.path.exists(path):
        return False
    with open(path, errors="replace") as f:
        return needle in f.read()


def _parent(store_url, run_dir):
    _wait_tcp(store_url, 30)  # etcd service is a hard prerequisite

    procs, files = {}, {}
    log = {r: os.path.join(run_dir, f"rank{r}.log") for r in (0, 1, 2)}
    try:
        # rank0 alone first: the election is first-come-first-served, so a solo head
        # start makes the FIRST leader/master deterministic
        procs[RANK_FAR_A], files[RANK_FAR_A] = _spawn("far0", run_dir, store_url, "rank0.log")
        _wait_alive(lambda: _grep(log[RANK_FAR_A], MARK_BECAME_LEADER), 60, "rank0 becomes leader",
                    {RANK_FAR_A: procs[RANK_FAR_A]}, run_dir)

        procs[RANK_NEAR], files[RANK_NEAR] = _spawn("near", run_dir, store_url, "rank1.log")
        procs[RANK_FAR_B], files[RANK_FAR_B] = _spawn("far2", run_dir, store_url, "rank2.log")
        _wait_file_alive(os.path.join(run_dir, f"far{RANK_FAR_B}_ready.json"), 90, "far2 ready",
                         dict(procs), run_dir)

        _wait_file_alive(os.path.join(run_dir, "near_acquired.json"), 180, "NEAR acquisition",
                         dict(procs), run_dir)
        print(f"[parent] killing rank {RANK_FAR_A} (store leader + master host, SIGKILL)", flush=True)
        procs[RANK_FAR_A].kill()  # true crash: lease expires, no clean handover
        procs[RANK_FAR_A].wait(timeout=30)
        _write(os.path.join(run_dir, "killed.json"), {"rank": RANK_FAR_A})

        # teardown ordering: the survivor FAR must uninitialize while a store host is
        # still alive; NEAR signals completion and WAITS before exiting
        _wait_file_alive(os.path.join(run_dir, "near_finished.json"), 300, "NEAR assertions done",
                         {RANK_NEAR: procs[RANK_NEAR], RANK_FAR_B: procs[RANK_FAR_B]}, run_dir)
        _write(os.path.join(run_dir, "shutdown.json"), {})
        procs[RANK_FAR_B].wait(timeout=90)
        assert procs[RANK_FAR_B].returncode == 0, f"survivor FAR failed: {procs[RANK_FAR_B].returncode}"

        _write(os.path.join(run_dir, "near_exit.json"), {})
        procs[RANK_NEAR].wait(timeout=90)
        assert procs[RANK_NEAR].returncode == 0, \
            f"NEAR failed: exitcode {procs[RANK_NEAR].returncode} (see {run_dir})"

        # ---- log assertions ----
        promotions = [r for r in (RANK_NEAR, RANK_FAR_B) if _grep(log[r], MARK_PROMOTION)]
        assert len(promotions) == 1, \
            f"expected exactly one re-election winner, got {promotions} (see {run_dir})"
        rediscovered = _grep(log[RANK_FAR_B], MARK_ENDPOINT_CHANGED) or \
            _grep(log[RANK_FAR_B], MARK_ENDPOINT_REFRESHED) or \
            _grep(log[RANK_NEAR], MARK_ENDPOINT_CHANGED) or _grep(log[RANK_NEAR], MARK_ENDPOINT_REFRESHED)
        assert rediscovered, f"no master re-discovery log on survivors (see {run_dir})"
        for r in (0, 1, 2):
            assert not _grep(log[r], MARK_ALREADY_WATCHED), f"rank {r}: stale single-waiter watch"

        print(f"(1/1) 06_ha_master_switchover: failover OK "
              f"(new leader/master on rank {promotions[0]}, placement recovered on rank {RANK_FAR_B})",
              flush=True)
        return 0
    finally:
        for f in files.values():
            f.close()


def main():
    args = sys.argv[1:]
    if args and args[0] in ("near", "far0", "far2"):  # child entry
        role, run_dir, store_url = args[0], args[1], args[2]
        if role == "near":
            _near_main(store_url, run_dir)
        else:
            _far_main(RANK_FAR_A if role == "far0" else RANK_FAR_B, store_url, run_dir)
        return

    store_url = args[0] if args else ETCD_STORE_URL
    import os
    run_dir = os.path.abspath("./log")
    os.makedirs(run_dir, exist_ok=True)
    print(f"[parent] run dir: {run_dir} (etcd={store_url})", flush=True)
    try:
        rc = _parent(store_url, run_dir)
    except Exception as e:
        print(f"[parent] FAIL: {e} — logs kept in {run_dir}", flush=True)
        raise
    if rc == 0 and not os.environ.get("MF_KEEP_LOGS"):
        shutil.rmtree(run_dir, ignore_errors=True)  # keep logs only on failure (MF_KEEP_LOGS=1 keeps them)


if __name__ == "__main__":
    main()
