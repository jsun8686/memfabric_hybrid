#!/usr/bin/env python3
# coding=utf-8
# Copyright: (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/PSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT ANY KIND OF EITHER EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FITNESS
# FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.
"""08: NEAR-side one-shot memory client.

Run it any time against a live FAR memory daemon (08_far_memory_daemon.py):
connect to the daemon's store, take a remote DRAM block via extend_remote_mem,
run a small copy matrix (device RDMA: local NPU HBM <-> remote FAR DRAM, i.e.
L2G/G2L) with an untimed round-trip probe plus one-way throughput per size,
release everything, exit 0. The daemon keeps serving the next client.
"""
import argparse
import socket
import sys
import time

import memfabric_hybrid as mf
from memfabric_hybrid import ralloc

DEFAULT_WORLD = 512          # declared world capacity, actual members join dynamically
NIC_PORT_BASE = 10005        # data-plane nic port base (set_nic); NOT the control rpc port
RPC_PORT_BASE = 11100        # control rpc port = base + rankId (smem_ralloc_def.h default)
POOL_WINDOW = 4 << 30        # pool window slot reported to the FAR placement master
DEFAULT_SIZES = "1M,8M"
DEFAULT_MB_PER_SIZE = 64     # one-way traffic per size
DEFAULT_REMOTE_MB = 64       # remote block size (must cover the largest size)
EXTEND_RETRY_SEC = 5
EXTEND_TIMEOUT_SEC = 300
GIB = 1 << 30

DATA_OP = ralloc.RallocDataOpType.DEVICE_RDMA   # dram media over the device-rdma transport
MEM_TYPE = ralloc.RallocMemType.HOST


def _log(msg):
    print(msg, flush=True)


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
    """4K-aligned NPU tensor of exactly nbytes: handle.register() requires 4K alignment."""
    import torch  # local import: only this client needs the torch backend
    buf = torch.empty((nbytes + 4096) // 4, dtype=torch.int32, device=device)
    off = ((-buf.data_ptr()) % 4096) // 4
    t = buf[off:off + nbytes // 4]
    if fill:
        torch.arange(nbytes // 4, dtype=torch.int32, device=device, out=t)
    return t


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--store", required=True,
                        help="store url of the FAR daemon, e.g. tcp://10.0.0.1:8587")
    parser.add_argument("--dev", type=int, required=True,
                        help="NPU id this client runs on")
    parser.add_argument("--sizes", default=DEFAULT_SIZES,
                        help=f"copy granularity list (default {DEFAULT_SIZES})")
    parser.add_argument("--mb-per-size", type=int, default=DEFAULT_MB_PER_SIZE,
                        help=f"one-way MB per size (default {DEFAULT_MB_PER_SIZE})")
    parser.add_argument("--remote-mb", type=int, default=DEFAULT_REMOTE_MB,
                        help=f"remote block size in MB, must cover the largest size (default {DEFAULT_REMOTE_MB})")
    parser.add_argument("--batch", action="store_true",
                        help="timed copies use one copy_data_batch per direction (submit the whole "
                             "matrix, single wait) instead of a copy_data loop")
    parser.add_argument("--world", type=int, default=DEFAULT_WORLD,
                        help=f"declared world capacity (default {DEFAULT_WORLD})")
    parser.add_argument("--rpc-port-base", type=int, default=RPC_PORT_BASE,
                        help=f"control rpc port base (default {RPC_PORT_BASE}); must match the daemon's value")
    args = parser.parse_args()

    sizes = [_parse_size(t) for t in args.sizes.split(",") if t.strip() != ""]
    if not sizes:
        raise RuntimeError("empty --sizes")
    remote_bytes = args.remote_mb << 20

    _wait_tcp(args.store, 30)
    dev = args.dev

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
        cfg.rpc_port_base = args.rpc_port_base
        cfg.set_nic(f"tcp://{socket.gethostbyname(socket.gethostname())}:{NIC_PORT_BASE}")
        assert ralloc.initialize(args.store, args.world, dev, cfg) == 0, "ralloc.initialize failed"
        ralloc_inited = True
        rank = ralloc.get_rank_id()

        handle = ralloc.create(id=0, max_dram_size=POOL_WINDOW, max_hbm_size=0, data_op_type=DATA_OP)

        # contributors may still be registering; extend fails until then
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
        mf.get_and_clear_last_err_msg()  # drain the sticky retry error from the loop above
        _log(f"[client rank {rank}] remote block from FAR rank {far_rank} (gva=0x{gva:x}, npu {dev})")

        device = f"npu:{dev}"
        for size in sizes:
            if size > remote_bytes:
                raise RuntimeError(f"size {size} exceeds remote block {remote_bytes}")
            src = _aligned_npu_tensor(size, device, fill=True)
            dst = _aligned_npu_tensor(size, device)
            # register user HBM into the pool so copy_data issues direct device-rdma
            # on these buffers instead of staging through a pool bounce copy
            assert handle.register(src.data_ptr(), size) == 0, "register src HBM failed"
            assert handle.register(dst.data_ptr(), size) == 0, "register dst HBM failed"
            # untimed correctness probe for this granularity
            assert handle.copy_data(src.data_ptr(), gva, size, 0) == 0, "probe L2G"
            assert handle.copy_data(gva, dst.data_ptr(), size, 0) == 0, "probe G2L"
            assert torch.equal(dst, src), "probe round-trip mismatch"

            slots = remote_bytes // size
            blocks = max(1, (args.mb_per_size << 20) // size)  # >=1 block even when size exceeds the budget
            one_way = blocks * size
            dst_offs = [gva + (i % slots) * size for i in range(blocks)]
            sz_list = [size] * blocks
            t0 = time.perf_counter()
            if args.batch:
                assert handle.copy_data_batch([src.data_ptr()] * blocks, dst_offs, sz_list,
                                               blocks, 0) == 0, "L2G batch"
            else:
                for off in dst_offs:
                    assert handle.copy_data(src.data_ptr(), off, size, 0) == 0, "L2G"
            tw = time.perf_counter() - t0
            t0 = time.perf_counter()
            if args.batch:
                assert handle.copy_data_batch(dst_offs, [dst.data_ptr()] * blocks, sz_list,
                                               blocks, 0) == 0, "G2L batch"
                assert torch.equal(dst, src), "batch round-trip mismatch"
            else:
                for off in dst_offs:
                    assert handle.copy_data(off, dst.data_ptr(), size, 0) == 0, "G2L"
            tr = time.perf_counter() - t0
            mode = "batch" if args.batch else "loop"
            _log(f"[client] size {size}: {blocks} blocks ({mode}), "
                 f"write {one_way / tw / GIB:.2f} GB/s ({tw / blocks * 1e6:.2f} us/block), "
                 f"read {one_way / tr / GIB:.2f} GB/s ({tr / blocks * 1e6:.2f} us/block) [round-trip OK]")
            assert handle.unregister(src.data_ptr()) == 0, "unregister src failed"
            assert handle.unregister(dst.data_ptr()) == 0, "unregister dst failed"

        handle.destroy()
        mf.get_and_clear_last_err_msg()
        assert mf.get_last_err_msg() == "", mf.get_last_err_msg()
        _log(f"[client rank {rank}] remote block released, exiting")
    finally:
        if ralloc_inited:
            ralloc.uninitialize(0)
        mf.uninitialize()
    print("(1/1) 08_near_memory_client: client OK", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
