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

--device switches the LOCAL endpoint of every copy from host DRAM (mmap) to
NPU HBM (torch_npu tensor DVA): copy_data's direction is inferred from the
address, so the same calls then exercise the LOCAL_DEVICE <-> GLOBAL_NVME
directions (aclrtMemcpy last hop). torch_npu is required only for this mode.
"""

import argparse
import ctypes
import mmap
import os
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


def _bind_numa_node(node):
    """pin the whole process to one NUMA node's CPUs. The NVMe op allocates its staging
    pools lazily on the copy thread (first-touch) and polls the qpair in it, so a single
    binding aligns thread + staging memory + poll with the data NIC's socket. Must run
    BEFORE mf.initialize: every thread spawned later inherits the affinity."""
    try:
        with open(f"/sys/devices/system/node/node{node}/cpulist") as f:
            raw = f.read().strip()
    except OSError as e:
        raise RuntimeError(f"--numa-node {node}: cannot read its cpulist: {e}") from e
    cpus = []
    for part in raw.split(","):
        if "-" in part:
            lo, hi = (int(x) for x in part.split("-"))
            cpus.extend(range(lo, hi + 1))
        else:
            cpus.append(int(part))
    os.sched_setaffinity(0, cpus)
    _log(f"[client] pinned to NUMA node {node} (cpus {raw}) -- staging/poll inherit the locality")


def _wait_tcp(url, timeout_sec):
    host, port = url.split("://", 1)[1].rsplit(":", 1)
    deadline = time.time() + timeout_sec
    while time.time() < deadline:
        with socket.socket() as s:
            if s.connect_ex((host, int(port))) == 0:
                return
        time.sleep(0.5)
    raise RuntimeError(f"endpoint not reachable within {timeout_sec}s: {url} — is the daemon up?")


def _init_torch_npu():
    """lazy import so the default host mode needs no torch; --device only"""
    try:
        import torch
        import torch_npu  # noqa: F401 -- registers the npu backend on import
        return torch
    except ImportError as e:
        raise RuntimeError("--device requires torch_npu (run on an NPU node)") from e


class _LocalBuf:
    """LOCAL endpoint of every copy. Host mode: anonymous mmap (LOCAL_HOST).
    --device: NPU HBM tensor (LOCAL_DEVICE, the DVA feeds copy_data unchanged,
    direction is inferred from the address -- no registration is needed: the
    NVMe op only stages through memcpy/aclrtMemcpy). fill()/read() hide the
    difference so the verification flow stays single-sourced."""

    def __init__(self, size, torch_mod=None):
        self.torch = torch_mod
        self.size = size
        if torch_mod is not None:
            self._t = torch_mod.empty(size, dtype=torch_mod.uint8, device="npu")
            self._mm = None
            self.addr = self._t.data_ptr()
        else:
            self._mm = mmap.mmap(-1, size)
            self._t = None
            self.addr = ctypes.addressof(ctypes.c_char.from_buffer(self._mm))

    def fill(self, data):
        assert len(data) == self.size
        if self._t is not None:
            self._t.copy_(self.torch.frombuffer(bytearray(data), dtype=self.torch.uint8).to("npu"))
            # torch's H2D rides its own stream and returns before the copy completes;
            # the NVMe op's aclrtMemcpy runs on a different stream with NO implicit
            # ordering between them -- sync the device or the staging D2H reads the
            # tensor's previous (uninitialized) contents
            self.torch.npu.synchronize()
        else:
            self._mm[:] = data

    def read(self):
        if self._t is not None:
            return bytes(self._t.cpu().numpy().tobytes())
        return bytes(self._mm[:])

    def close(self):
        if self._mm is not None:
            self._mm.close()
        # the tensor is freed when the last reference (self._t) drops


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
    parser.add_argument("--device", action="store_true",
                        help="route the LOCAL endpoint of every copy through NPU HBM (torch_npu "
                             "tensor DVA): exercises the LOCAL_DEVICE <-> GLOBAL_NVME directions, "
                             "default off (host mmap, LOCAL_HOST)")
    parser.add_argument("--numa-node", type=int, default=None, metavar="N",
                        help="pin the client to NUMA node N (aligns thread + staging + poll with "
                             "the data NIC's socket; this cluster's data NICs sit on node 6), "
                             "default off (scheduler picks randomly)")
    parser.add_argument("--no-zc", action="store_true",
                        help="skip pool.register zero-copy pre-registration "
                             "(staging baseline for A/B comparison)")
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
    # dlopen resolves the search path at process start: a missing path cannot be fixed
    # later from python, so fail loudly BEFORE the pool setup walks into it
    ld_path = os.environ.get("LD_LIBRARY_PATH", "")
    if "spdk" not in ld_path:
        _log("[client] WARN: LD_LIBRARY_PATH contains no spdk library dir -- the first NVMe IO "
             "will fail dlopen(libspdk_env_dpdk.so). Export both <spdk>/build/lib and "
             "<spdk>/dpdk/build/lib before launching.")
    _wait_tcp(args.store, 30)

    if args.numa_node is not None:
        _bind_numa_node(args.numa_node)

    mf.set_log_level(1)
    assert mf.initialize() == 0, "mf.initialize failed"
    ralloc_inited = False
    pool = None
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

        pool = ralloc.create(id=0, max_dram_size=0, max_nvme_size=max_nvme,
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
        # local endpoint: host mmap (LOCAL_HOST) or NPU HBM DVA (LOCAL_DEVICE, --device);
        # copy_data infers the direction from the address either way
        tmod = _init_torch_npu() if args.device else None
        seed = 0x16 + rank
        pattern = _make_pattern(size, seed)
        w = _LocalBuf(size, tmod)
        r = _LocalBuf(size, tmod)
        bufs = [w, r]
        if args.device:
            _log(f"[client] device endpoints: w=0x{w.addr:x} r=0x{r.addr:x} "
                 f"(must sit inside the NPU virtual address window)")
        w.fill(pattern)
        _copy(pool, w.addr, nvme_gva, size, "write pattern -> NVMe")
        r.fill(bytes(size))
        _copy(pool, nvme_gva, r.addr, size, "read NVMe -> buffer")
        got = r.read()
        if got != pattern:
            import numpy as np
            off = int(np.argmax(np.frombuffer(got, dtype=np.uint8) != np.frombuffer(pattern, dtype=np.uint8)))
            raise AssertionError(f"aligned bulk round-trip mismatch at offset {off}: "
                                 f"got={got[off:off + 16].hex()} want={pattern[off:off + 16].hex()}")
        _log(f"[client] aligned {size}-byte round-trip OK (head/tail blocks distinct)")

        # ---- 3) sub-sector RMW: 10 bytes at offset +100 must merge, neighbours survive ----
        magic = b"MEMFABRIC!"
        m = _LocalBuf(64, tmod)
        v = _LocalBuf(64, tmod)
        bufs += [m, v]
        m.fill(magic.ljust(64, b"\x00"))
        _copy(pool, m.addr, nvme_gva + 100, 10, "sub-sector write @+100")
        v.fill(bytes(64))
        _copy(pool, nvme_gva + 64, v.addr, 64, "read window @+64")
        window = v.read()
        want = pattern[64:100] + magic + pattern[110:128]
        assert window[:36 + 10 + 18] == want, \
            f"RMW merge mismatch: got={window[:64].hex()} want={want.hex()}"
        _log("[client] sub-sector RMW OK (10B magic @+100 merged, neighbours preserved)")

        # ---- 4) zero-copy pre-registration (B3, host buffers only) ----
        # pool.register pins the 2MB-aligned inner window of each buffer with
        # spdk_mem_register -> later copies DMA the user buffer directly, the
        # staging memcpy disappears. Falls back to staging automatically when
        # unsupported; --no-zc keeps the staging baseline for A/B comparison.
        zc_mode = "off (--no-zc, staging baseline)" if args.no_zc else "on"
        if not args.no_zc and not args.device:
            for b in (w, r):
                pool.register(b.addr, size)
            _log(f"[client] zero-copy: w/r buffers registered ({zc_mode})")
        else:
            _log(f"[client] zero-copy: {zc_mode}")

        # ---- 5) bandwidth matrix (write and read, plain unified API) ----
        mode = "device" if args.device else "host"
        _log(f"[client] bandwidth matrix ({'/'.join(str(s) for s in bw_sizes)} bytes, "
             f"local endpoint {mode}):")
        _log("    size      write GB/s   read GB/s")
        for s in bw_sizes:
            if s > size:
                _log(f"    {s:>9}  skipped (block is {size} bytes)")
                continue
            n = max(3, min(64, (64 << 20) // s))
            _copy(pool, w.addr, nvme_gva, s, "bw warmup")  # also warms the endpoint/qpair cache
            t0 = time.perf_counter()
            for _ in range(n):
                _copy(pool, w.addr, nvme_gva, s, "bw write")
            t_wr = time.perf_counter() - t0
            _copy(pool, nvme_gva, r.addr, s, "bw warmup")
            t0 = time.perf_counter()
            for _ in range(n):
                _copy(pool, nvme_gva, r.addr, s, "bw read")
            t_rd = time.perf_counter() - t0
            _log(f"    {s:>9}  {n * s / t_wr / GIB:10.2f}   {n * s / t_rd / GIB:9.2f}")

        # release the registrations before the buffers close (DMA-safety contract)
        if not args.no_zc and not args.device:
            for b in (w, r):
                pool.unregister(b.addr)
        pool.destroy()
        pool = None
        mf.get_and_clear_last_err_msg()
        _log(f"[client rank {rank}] pool destroyed, exiting")
    finally:
        # destroy on the failure path too, BEFORE the global uninit (a pool object
        # surviving into GC after ralloc.uninitialize trips Assert g_smemRallocInited)
        if pool is not None:
            pool.destroy()
            pool = None
        for b in bufs:
            b.close()
        if ralloc_inited:
            ralloc.uninitialize(0)
        mf.uninitialize()
    _log("[client] NVMe SSD pool example finished cleanly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
