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
import sys
import time

import torch

import memfabric_hybrid as mf
from memfabric_hybrid import ralloc

STORE_PORT = 8572
ONE_GIB = 1 << 30  # 1GB window slot per rank
EXTEND_REMOTE_BYTES = 64 * 1024 * 1024  # 64MB block contributed by the FAR node
COPY_BYTES = 4 * 1024 * 1024  # 4MB int32 payload
WORLD_SIZE = 2

# head retries extend_remote until the FAR contributor registers with the master
FAR_WAIT_TIMEOUT_SEC = 600
FAR_WAIT_RETRY_SEC = 5


def _run_near(head_node_ip: str) -> None:
    store_url = f"tcp://{head_node_ip}:{STORE_PORT}"
    mf.set_log_level(3)
    assert mf.initialize() == 0, "mf.initialize failed"
    ralloc_inited = False
    try:
        cfg = ralloc.RallocConfig()
        cfg.rank_id = 0
        cfg.auto_ranking = False  # fixed identities: head is rank 0, node B is rank 1
        cfg.role = ralloc.RallocRole.NEAR
        cfg.start_store = True  # head hosts the store; master service runs in this process
        cfg.set_nic("tcp://127.0.0.1:10005")
        assert ralloc.initialize(store_url, WORLD_SIZE, 0, cfg) == 0, "ralloc.initialize failed"
        ralloc_inited = True

        handle = ralloc.create(
            id=0,
            max_dram_size=ONE_GIB,
            data_op_type=ralloc.RallocDataOpType.HOST_RDMA,
        )
        print(f"[near] pool created (store={store_url}) — waiting for the FAR contributor on node B ...", flush=True)

        ret, info = 0, {"rank_id": 0xFFFFFFFF, "gva": 0}
        deadline = time.time() + FAR_WAIT_TIMEOUT_SEC
        while True:
            ret, info = handle.extend_remote_mem(ralloc.RallocMemType.HOST, EXTEND_REMOTE_BYTES)
            if ret == 0:
                break
            if time.time() > deadline:
                raise RuntimeError(f"no FAR candidate registered in {FAR_WAIT_TIMEOUT_SEC}s: {ret}")
            print("[near] no FAR candidate yet (start rank 1 on node B), retrying ...", flush=True)
            time.sleep(FAR_WAIT_RETRY_SEC)
        assert info["rank_id"] == 1 and info["gva"] != 0, f"extend_remote_mem: {ret} {info}"
        mf.get_and_clear_last_err_msg()
        gva = info["gva"]
        print(f"[near] remote block acquired from rank {info['rank_id']} (gva=0x{gva:x})", flush=True)

        src = torch.arange(COPY_BYTES // 4, dtype=torch.int32).contiguous()
        assert handle.copy_data(src.data_ptr(), gva, COPY_BYTES, 0) == 0, "H2G into far slot"
        got = torch.empty(COPY_BYTES // 4, dtype=torch.int32)
        assert handle.copy_data(gva, got.data_ptr(), COPY_BYTES, 0) == 0, "G2H from far slot"
        assert torch.equal(got, src), "round-trip via far block"
        assert handle.get_group_ranks() == [0, 1], "group ranks"
        assert handle.get_mem_size_by_rank(1) >= EXTEND_REMOTE_BYTES, "far slot size"
        print("[near] round-trip via FAR block OK — sleeping until Ctrl+C", flush=True)
        try:
            while True:
                time.sleep(60)
        except KeyboardInterrupt:
            print("[near] interrupted, destroying pool", flush=True)

        assert handle.destroy() == 0, "destroy pool"
        assert mf.get_last_err_msg() == "", mf.get_last_err_msg()
    finally:
        if ralloc_inited:
            ralloc.uninitialize(0)
        mf.uninitialize()
    print("[near] cleanup done", flush=True)


def _run_far(head_node_ip: str) -> None:
    store_url = f"tcp://{head_node_ip}:{STORE_PORT}"
    mf.set_log_level(3)
    assert mf.initialize() == 0, "mf.initialize failed"
    ralloc_inited = False
    try:
        cfg = ralloc.RallocConfig()
        cfg.rank_id = 1
        cfg.auto_ranking = False  # fixed identities: head is rank 0, node B is rank 1
        cfg.role = ralloc.RallocRole.FAR
        cfg.start_store = False
        cfg.set_nic("tcp://127.0.0.1:10005")
        assert ralloc.initialize(store_url, WORLD_SIZE, 0, cfg) == 0, "ralloc.initialize failed"
        ralloc_inited = True
        print(f"[far] contributor ready (registered with master at {store_url})", flush=True)
        try:
            while True:
                time.sleep(60)
        except KeyboardInterrupt:
            print("[far] interrupted", flush=True)
    finally:
        if ralloc_inited:
            ralloc.uninitialize(0)
        mf.uninitialize()
    print("[far] cleanup done", flush=True)


def main() -> None:
    if len(sys.argv) < 2 or sys.argv[1] not in ("0", "1"):
        raise RuntimeError("usage: python3 04_multi_node_ralloc.py <0|1> [head_ip]")
    head_ip = (sys.argv[2] if len(sys.argv) > 2 else input("Head node IP: ")).strip()
    if not head_ip:
        raise RuntimeError("head node IP required")
    if int(sys.argv[1]) == 0:
        _run_near(head_ip)
    else:
        _run_far(head_ip)


if __name__ == "__main__":
    main()
