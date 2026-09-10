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
import multiprocessing as mp
import sys

import torch

import memfabric_hybrid as mf
from memfabric_hybrid import ralloc

ONE_GIB = 1 << 30  # 1GB window slot per rank
EXTEND_LOCAL_BYTES = 32 * 1024 * 1024   # 32MB block on the NEAR local slot
EXTEND_REMOTE_BYTES = 64 * 1024 * 1024  # 64MB block contributed by the FAR node
EXTEND_REMOTE_AGAIN = 32 * 1024 * 1024  # 32MB second remote block (executor extend branch)
COPY_BYTES = 4 * 1024 * 1024  # 4MB int32 payload
STORE_URL = "tcp://127.0.0.1:8572"
WORLD_SIZE = 2

RANK_FAR, DEVICE_ID = 0, 0
RANK_NEAR = 1


def _media_config(media):
    """host (default): HOST media via HOST_RDMA; device: HBM media via SDMA (NPU required).
    The device variant reserves an HBM-only window: on A2(910B) SoCs a DRAM window cannot carry the
    SDMA bit — the hybm conn-based dram segment is not sdma-reachable (910C/GVA_V4 unified VA除外)."""
    if media == "device":
        return (ralloc.RallocMemType.DEVICE,
                ralloc.RallocDataOpType.SDMA,
                0, ONE_GIB)
    return ralloc.RallocMemType.HOST, ralloc.RallocDataOpType.HOST_RDMA, ONE_GIB, 0


def _wait_for_store(timeout_sec=60.0):
    """NEAR needs the FAR-hosted store (and its master key) before ralloc.initialize."""
    import socket
    import time

    host, port = STORE_URL.split("://", 1)[1].rsplit(":", 1)
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        with socket.socket() as s:
            if s.connect_ex((host, int(port))) == 0:
                return
        time.sleep(0.5)
    raise RuntimeError(f"config store not reachable within {timeout_sec}s: {STORE_URL}")


def _near_main(sync: mp.Barrier, media: str):
    mem_type, data_op, max_dram, max_hbm = _media_config(media)
    mf.set_log_level(3)
    assert mf.initialize() == 0, "mf.initialize failed"
    ralloc_inited = False
    try:
        cfg = ralloc.RallocConfig()
        cfg.rank_id = RANK_NEAR
        cfg.auto_ranking = False
        cfg.role = ralloc.RallocRole.NEAR
        cfg.start_store = False  # FAR (rank 0) hosts the store and the master service
        cfg.set_nic("tcp://174.111.50.202:10005")
        _wait_for_store()  # master discovery inside initialize needs the FAR-hosted store
        assert ralloc.initialize(STORE_URL, WORLD_SIZE, DEVICE_ID, cfg) == 0, "ralloc.initialize failed"
        ralloc_inited = True
        print(f"[rank {RANK_NEAR}] ralloc initialized (NEAR, store={STORE_URL})", flush=True)

        sync.wait()  # (1/5) both sides initialized; the store-host master seeded itself already
        handle = ralloc.create(
            id=0,
            max_dram_size=max_dram,
            max_hbm_size=max_hbm,
            data_op_type=data_op,
        )
        print(f"[rank {RANK_NEAR}] (2/5) pool created (pure alignment, no local commit)", flush=True)

        # Phase 1: extend one block on the local NEAR slot
        ret, info = handle.extend_local_mem(mem_type, EXTEND_LOCAL_BYTES)
        assert ret == 0 and info["rank_id"] == RANK_NEAR and info["gva"] != 0, f"extend_local_mem: {ret} {info}"
        assert handle.get_mem_size_by_rank(RANK_NEAR, mem_type) == EXTEND_LOCAL_BYTES, "local slot size"
        print(f"[rank {RANK_NEAR}] (3/5) extend_local_mem OK (gva=0x{info['gva']:x})", flush=True)

        # Phase 2: acquire a remote block from the FAR contributor via master placement
        ret, info = handle.extend_remote_mem(mem_type, EXTEND_REMOTE_BYTES)
        assert ret == 0 and info["rank_id"] == RANK_FAR and info["gva"] != 0, f"extend_remote_mem: {ret} {info}"
        gva_remote = info["gva"]
        assert sorted(handle.get_group_ranks()) == sorted([RANK_NEAR, RANK_FAR]), "group ranks"
        assert handle.get_mem_size_by_rank(RANK_FAR, mem_type) >= EXTEND_REMOTE_BYTES, "far slot size"
        slot_base = handle.get_mem_ptr_by_rank(RANK_FAR, mem_type)
        assert slot_base != 0 and slot_base <= gva_remote < slot_base + ONE_GIB, \
            f"gva outside far slot: base=0x{slot_base:x} gva=0x{gva_remote:x}"

        src = torch.arange(COPY_BYTES // 4, dtype=torch.int32).contiguous()
        assert handle.copy_data(src.data_ptr(), gva_remote, COPY_BYTES, 0) == 0, "H2G into far slot"
        got = torch.empty(COPY_BYTES // 4, dtype=torch.int32)
        assert handle.copy_data(gva_remote, got.data_ptr(), COPY_BYTES, 0) == 0, "G2H from far slot"
        if media == "device":
            assert handle.wait() == 0, "wait for async SDMA copies"
        assert torch.equal(got, src), "round-trip via far block"
        print(f"[rank {RANK_NEAR}] (4/5) extend_remote_mem + round-trip OK (contributor rank {RANK_FAR})", flush=True)

        # Phase 3: second remote acquire hits the executor extend branch, LB accounting refreshed
        ret, info = handle.extend_remote_mem(mem_type, EXTEND_REMOTE_AGAIN)
        assert ret == 0 and info["rank_id"] == RANK_FAR and info["gva"] != 0, f"extend_remote_mem again: {ret} {info}"
        assert handle.get_mem_size_by_rank(RANK_FAR, mem_type) >= EXTEND_REMOTE_BYTES + EXTEND_REMOTE_AGAIN, \
            "far slot size"

        sync.wait()  # (5/5) FAR side may finish once the phases are done
        handle.destroy()
        sync.wait()  # leave events settled before the FAR side tears down its executor entries
        assert mf.get_last_err_msg() == "", mf.get_last_err_msg()
    finally:
        if ralloc_inited:
            ralloc.uninitialize(0)
        mf.uninitialize()


def _far_main(sync: mp.Barrier, media: str):
    mf.set_log_level(3)
    assert mf.initialize() == 0, "mf.initialize failed"
    ralloc_inited = False
    try:
        cfg = ralloc.RallocConfig()
        cfg.rank_id = RANK_FAR
        cfg.auto_ranking = False
        cfg.role = ralloc.RallocRole.FAR
        cfg.start_store = True  # rank 0 hosts the store; master seeds itself, loopback accounting
        cfg.set_nic("tcp://174.111.50.202:10005")
        assert ralloc.initialize(STORE_URL, WORLD_SIZE, DEVICE_ID, cfg) == 0, "ralloc.initialize failed"
        ralloc_inited = True
        print(f"[rank {RANK_FAR}] ralloc initialized (FAR: store host + master + contributor)", flush=True)

        sync.wait()  # master seeded itself as candidate, NEAR may start PLACEMENT now
        sync.wait()  # stay alive while the NEAR side runs its phases
        sync.wait()  # leave events settled, then tear down
        assert mf.get_last_err_msg() == "", mf.get_last_err_msg()
    finally:
        if ralloc_inited:
            ralloc.uninitialize(0)
        mf.uninitialize()


def main():
    media = sys.argv[1].lower() if len(sys.argv) > 1 else "host"
    if media not in ("host", "device"):
        raise RuntimeError("usage: python 03_single_node_multi_process_ralloc.py [host|device]")
    mp.set_start_method("spawn", force=True)
    sync = mp.Barrier(WORLD_SIZE)

    p_near = mp.Process(target=_near_main, args=(sync, media))
    p_far = mp.Process(target=_far_main, args=(sync, media))

    p_near.start()
    p_far.start()
    p_near.join()
    p_far.join()

    if p_near.exitcode != 0 or p_far.exitcode != 0:
        raise RuntimeError(f"child failed: near={p_near.exitcode}, far={p_far.exitcode}")
    print(f"(5/5) 03_single_node_multi_process_ralloc [{media}]: NEAR+FAR OK", flush=True)


if __name__ == "__main__":
    main()
