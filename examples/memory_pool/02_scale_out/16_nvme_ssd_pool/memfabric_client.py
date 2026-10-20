#!/usr/bin/env python3
# coding=utf-8
# Copyright: (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
"""NVMe SSD pool client (NEAR role) for example 16.

Flow: create a pure-NVMe pool (single media, no DRAM/HBM window) with the
transport-explicit op bit -> extend_remote_mem(NVME) to get an LBA extent
contributed by the FAR node -> byte-granular write/read verification through
the plain unified copy API (aligned bulk copy + sub-sector RMW) -> a
read/write bandwidth matrix.

The whole IO path is host-plane (no NPU/torch needed, no transport library:
the NVMe op bit never triggers LoadExtendLibrary). The NVMe-oF transport is
declared by the pool op bit (--nvme-transport tcp|rdma) and verified against
the target listener on the FAR side. The NEAR process dlopens the SPDK
initiator libraries on the first NVMe IO:
    export LD_LIBRARY_PATH=/path/to/spdk/build/lib:$LD_LIBRARY_PATH
"""

import argparse
import ctypes
import mmap
import socket
import sys
import time

import memfabric_hybrid as mf
from memfabric_hybrid import ralloc

DEFAULT_WORLD = 512            # declared world capacity, actual members join dynamically
NIC_PORT_BASE = 10005          # data-plane nic port base (set_nic); NOT the control rpc port
RPC_PORT_BASE = 11100          # control rpc port = base + rankId (smem_ralloc_def.h default)
DEFAULT_SIZE = "64M"           # NVMe block bytes acquired from the FAR contributor
DEFAULT_MAX_NVME = "1G"        # pool NVMe window (per-rank slot VA reservation)
DEFAULT_BW_SIZES = "4K,64K,1M,16M"
EXTEND_RETRY_SEC = 5
EXTEND_TIMEOUT_SEC = 300
GIB = 1 << 30
LARGE_PAGE = 2 << 20           # 2M: block/window alignment rule of the pool

DATA_OP_NVME = {"tcp": ralloc.RallocDataOpType.NVME_TCP,
                "rdma": ralloc.RallocDataOpType.NVME_RDMA}


def _log(msg):
    print(msg, flush=True)


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


def _wait_tcp(url, timeout_sec):
    host, port = url.split("://", 1)[1].rsplit(":", 1)
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        with socket.socket() as s:
            if s.connect_ex((host, int(port))) == 0:
                return
        time.sleep(0.5)
    raise RuntimeError(f"endpoint not reachable within {timeout_sec}s: {url} — is the daemon up?")


def _map_buffer(size):
    """anonymous page-aligned host buffer: the LOCAL_HOST endpoint of the copies"""
    mm = mmap.mmap(-1, size)
    addr = ctypes.addressof(ctypes.c_char.from_buffer(mm))
    return mm, addr


def _make_pattern(size, seed):
    """C-speed replicated 4K pattern with distinct head/tail blocks (misalignment detector)"""
    base = bytes((i * 131 + seed) & 0xFF for i in range(4096))
    if size <= 4096:
        return base[:size]
    buf = bytearray(base * (size // 4096))
    tail = bytes((b ^ 0x5A) & 0xFF for b in base)
    buf[size - 4096:] = tail
    return bytes(buf)


def _copy(handle, src, dst, size, tag):
    ret = handle.copy_data(src, dst, size, 0)
    assert ret == 0, f"copy_data({tag}) failed: {ret} — {mf.get_and_clear_last_err_msg()}"


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--store", required=True,
                        help="store url of the FAR daemon, e.g. tcp://10.0.0.1:8588")
    parser.add_argument("--dev", type=int, default=0, help="NPU id this client runs on (default 0)")
    parser.add_argument("--size", default=DEFAULT_SIZE,
                        help=f"NVMe block bytes acquired from the FAR contributor, 2M aligned "
                             f"(K/M/G suffix, default {DEFAULT_SIZE})")
    parser.add_argument("--max-nvme", default=DEFAULT_MAX_NVME,
                        help=f"pool NVMe window per rank slot, 2M aligned (default {DEFAULT_MAX_NVME})")
    parser.add_argument("--bw-sizes", default=DEFAULT_BW_SIZES,
                        help=f"comma list of copy sizes for the bandwidth matrix (default {DEFAULT_BW_SIZES})")
    parser.add_argument("--nvme-transport", choices=["tcp", "rdma"], default="tcp",
                        help="NVMe-oF transport declared by the pool op bit and verified against the "
                             "target listener on the FAR side, default tcp (loopback simulation)")
    parser.add_argument("--world", type=int, default=DEFAULT_WORLD,
                        help=f"declared world capacity (default {DEFAULT_WORLD})")
    parser.add_argument("--rpc-port-base", type=int, default=RPC_PORT_BASE,
                        help=f"control rpc port base (default {RPC_PORT_BASE}); must match the daemon's value")
    args = parser.parse_args()

    size = _parse_size(args.size)
    max_nvme = _parse_size(args.max_nvme)
    bw_sizes = [_parse_size(t) for t in args.bw_sizes.split(",") if t.strip()]
    if size % LARGE_PAGE != 0:
        raise RuntimeError(f"--size ({size}) must be {LARGE_PAGE} aligned")
    if max_nvme % LARGE_PAGE != 0 or size > max_nvme:
        raise RuntimeError(f"--max-nvme ({max_nvme}) must be {LARGE_PAGE} aligned and >= --size ({size})")

    _log(f"[client] store: {args.store}, size: {size}, max_nvme: {max_nvme}")
    _log(f"[client] nvme-transport: {args.nvme_transport}")
    _wait_tcp(args.store, 30)

    mf.set_log_level(1)
    assert mf.initialize() == 0, "mf.initialize failed"
    ralloc_inited = False
    bufs = []
    try:
        cfg = ralloc.RallocConfig()
        cfg.auto_ranking = True
        cfg.role = ralloc.RallocRole.NEAR
        cfg.start_store = False
        cfg.dynamic_world_size = True
        cfg.rpc_port_base = args.rpc_port_base
        cfg.set_nic(f"tcp://{socket.gethostbyname(socket.gethostname())}:{NIC_PORT_BASE}")
        assert ralloc.initialize(args.store, args.world, args.dev, cfg) == 0, "ralloc.initialize failed"
        ralloc_inited = True
        rank = ralloc.get_rank_id()

        pool = ralloc.create(id=0, max_nvme_size=max_nvme,
                             data_op_type=DATA_OP_NVME[args.nvme_transport])
        _log(f"[client rank {rank}] pure-NVMe pool created (NVMe/{args.nvme_transport}, nvme={max_nvme})")

        # ---- 1) NVMe block from the FAR contributor (master placement, never self) ----
        deadline = time.time() + EXTEND_TIMEOUT_SEC
        while True:
            ret, info = pool.extend_remote_mem(ralloc.RallocMemType.NVME, size)
            if ret == 0 and info.get("gva"):
                break
            if time.time() > deadline:
                raise RuntimeError(f"no FAR NVMe candidate within {EXTEND_TIMEOUT_SEC}s: {ret} {info}")
            time.sleep(EXTEND_RETRY_SEC)
        far_rank = info["rank_id"]
        nvme_gva = info["gva"]
        _log(f"[client rank {rank}] NVMe block from FAR rank {far_rank}: gva=0x{nvme_gva:x}, {size} bytes")

        # ---- 2) byte-granular write/read verification (aligned bulk) ----
        seed = 0x16 + rank
        pattern = _make_pattern(size, seed)
        w_mm, w_addr = _map_buffer(size)
        r_mm, r_addr = _map_buffer(size)
        bufs = [w_mm, r_mm]
        w_mm[:] = pattern
        _copy(pool, w_addr, nvme_gva, size, "write pattern -> NVMe")
        r_mm[:] = bytes(size)
        _copy(pool, nvme_gva, r_addr, size, "read NVMe -> buffer")
        assert bytes(r_mm[:]) == pattern, "aligned bulk round-trip mismatch"
        _log(f"[client] aligned {size}-byte round-trip OK (head/tail blocks distinct)")

        # ---- 3) sub-sector RMW: 10 bytes at offset +100 must merge, neighbours survive ----
        magic = b"MEMFABRIC!"
        m_mm, m_addr = _map_buffer(64)
        v_mm, v_addr = _map_buffer(64)
        bufs += [m_mm, v_mm]
        m_mm[:10] = magic
        _copy(pool, m_addr, nvme_gva + 100, 10, "sub-sector write @+100")
        _copy(pool, nvme_gva + 64, v_addr, 64, "read window @+64")
        window = bytes(v_mm[:])
        want = pattern[64:100] + magic + pattern[110:128]
        assert window[:36 + 10 + 18] == want, \
            f"RMW merge mismatch: got={window[:64].hex()} want={want.hex()}"
        _log("[client] sub-sector RMW OK (10B magic @+100 merged, neighbours preserved)")

        # ---- 4) bandwidth matrix (write and read, plain unified API) ----
        _log(f"[client] bandwidth matrix ({'/'.join(str(s) for s in bw_sizes)} bytes):")
        _log("    size      write GB/s   read GB/s")
        for s in bw_sizes:
            if s > size:
                _log(f"    {s:>9}  skipped (block is {size} bytes)")
                continue
            n = max(3, min(64, (64 << 20) // s))
            _copy(pool, w_addr, nvme_gva, s, "bw warmup")  # also warms the endpoint/qpair cache
            t0 = time.perf_counter()
            for _ in range(n):
                _copy(pool, w_addr, nvme_gva, s, "bw write")
            t_wr = time.perf_counter() - t0
            _copy(pool, nvme_gva, r_addr, s, "bw warmup")
            t0 = time.perf_counter()
            for _ in range(n):
                _copy(pool, nvme_gva, r_addr, s, "bw read")
            t_rd = time.perf_counter() - t0
            _log(f"    {s:>9}  {n * s / t_wr / GIB:10.2f}   {n * s / t_rd / GIB:9.2f}")

        pool.destroy()
        mf.get_and_clear_last_err_msg()
        _log(f"[client rank {rank}] pool destroyed, exiting")
    finally:
        for b in bufs:
            b.close()
        if ralloc_inited:
            ralloc.uninitialize(0)
        mf.uninitialize()
    _log("[client] NVMe SSD pool example finished cleanly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
